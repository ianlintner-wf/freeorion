#include "ShipDesign.h"

#include "../util/OptionsDB.h"
#include "../util/Logger.h"
#include "../util/AppInterface.h"
#include "../util/MultiplayerCommon.h"
#include "../util/CheckSums.h"
#include "../util/ScopedTimer.h"
#include "../parse/Parse.h"
#include "../Empire/Empire.h"
#include "../Empire/EmpireManager.h"
#include "Condition.h"
#include "Effect.h"
#include "Planet.h"
#include "Ship.h"
#include "Predicates.h"
#include "Species.h"
#include "Universe.h"
#include "ValueRef.h"
#include "Enums.h"

#include <cfloat>
#include <unordered_set>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/uuid/nil_generator.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/lexical_cast.hpp>

extern FO_COMMON_API const int INVALID_DESIGN_ID = -1;

using boost::io::str;

namespace {
    void AddRules(GameRules& rules) {
        // makes all ships cost 1 PP and take 1 turn to produce
        rules.Add<bool>("RULE_CHEAP_AND_FAST_SHIP_PRODUCTION",
                        "RULE_CHEAP_AND_FAST_SHIP_PRODUCTION_DESC",
                        "", false, true);
        rules.Add<double>("RULE_SHIP_SPEED_FACTOR", "RULE_SHIP_SPEED_FACTOR_DESC",
                          "BALANCE", 1.0, true, RangedValidator<double>(0.1, 10.0));
        rules.Add<double>("RULE_SHIP_STRUCTURE_FACTOR", "RULE_SHIP_STRUCTURE_FACTOR_DESC",
                          "BALANCE", 1.0, true, RangedValidator<double>(0.1, 10.0));
    }
    bool temp_bool = RegisterGameRules(&AddRules);

    const std::string EMPTY_STRING;

    // create effectsgroup that increases the value of \a meter_type
    // by the result of evalulating \a increase_vr
    std::shared_ptr<Effect::EffectsGroup>
    IncreaseMeter(MeterType meter_type,
                  ValueRef::ValueRefBase<double>* increase_vr)
    {
        typedef std::shared_ptr<Effect::EffectsGroup> EffectsGroupPtr;
        typedef std::vector<Effect::EffectBase*> Effects;
        Condition::Source* scope = new Condition::Source;
        Condition::Source* activation = new Condition::Source;

        ValueRef::ValueRefBase<double>* vr =
            new ValueRef::Operation<double>(
                ValueRef::PLUS,
                new ValueRef::Variable<double>(ValueRef::EFFECT_TARGET_VALUE_REFERENCE, std::vector<std::string>()),
                increase_vr
            );
        return EffectsGroupPtr(
            new Effect::EffectsGroup(
                scope, activation, Effects(1, new Effect::SetMeter(meter_type, vr))));
    }

    // create effectsgroup that increases the value of \a meter_type
    // by the specified amount \a fixed_increase
    std::shared_ptr<Effect::EffectsGroup>
    IncreaseMeter(MeterType meter_type, float fixed_increase) {
        ValueRef::ValueRefBase<double>* increase_vr =
            new ValueRef::Constant<double>(fixed_increase);
        return IncreaseMeter(meter_type, increase_vr);
    }

    // create effectsgroup that increases the value of \a meter_type
    // by the product of \a base_increase and the value of the game
    // rule of type double with the name \a scaling_factor_rule_name
    std::shared_ptr<Effect::EffectsGroup>
    IncreaseMeter(MeterType meter_type, float base_increase,
                  const std::string& scaling_factor_rule_name)
    {
        // if no rule specified, revert to fixed constant increase
        if (scaling_factor_rule_name.empty())
            return IncreaseMeter(meter_type, base_increase);

        ValueRef::ValueRefBase<double>* increase_vr = new ValueRef::Operation<double>(
            ValueRef::TIMES,
            new ValueRef::Constant<double>(base_increase),
            new ValueRef::ComplexVariable<double>(
                "GameRule", nullptr, nullptr, nullptr,
                new ValueRef::Constant<std::string>(scaling_factor_rule_name)
            )
        );

        return IncreaseMeter(meter_type, increase_vr);
    }

    // create effectsgroup that increases the value of the part meter
    // of type \a meter_type for part name \a part_name by the fixed
    // amount \a increase
    std::shared_ptr<Effect::EffectsGroup>
    IncreaseMeter(MeterType meter_type, const std::string& part_name,
                  float increase, bool allow_stacking = true)
    {
        typedef std::shared_ptr<Effect::EffectsGroup> EffectsGroupPtr;
        typedef std::vector<Effect::EffectBase*> Effects;
        Condition::Source* scope = new Condition::Source;
        Condition::Source* activation = new Condition::Source;

        ValueRef::ValueRefBase<double>* value_vr =
            new ValueRef::Operation<double>(
                ValueRef::PLUS,
                new ValueRef::Variable<double>(ValueRef::EFFECT_TARGET_VALUE_REFERENCE, std::vector<std::string>()),
                new ValueRef::Constant<double>(increase)
            );

        ValueRef::ValueRefBase<std::string>* part_name_vr =
            new ValueRef::Constant<std::string>(part_name);

        std::string stacking_group = (allow_stacking ? "" :
            (part_name + "_" + boost::lexical_cast<std::string>(meter_type) + "_PartMeter"));

        return EffectsGroupPtr(
            new Effect::EffectsGroup(
                scope, activation,
                Effects(1, new Effect::SetShipPartMeter(meter_type, part_name_vr, value_vr)),
                part_name, stacking_group));
    }

    bool DesignsTheSame(const ShipDesign& one, const ShipDesign& two) {
        return (
            one.Name()              == two.Name() &&
            one.Description()       == two.Description() &&
            one.DesignedOnTurn()    == two.DesignedOnTurn() &&
            one.Hull()              == two.Hull() &&
            one.Parts()             == two.Parts() &&
            one.Icon()              == two.Icon() &&
            one.Model()             == two.Model()
        );
        // not checking that IDs are the same, since the purpose of this is to
        // check if a design that might be added to the universe (which doesn't
        // have an ID yet) is the same as one that has already been added
        // (which does have an ID)
    }
}

namespace CheckSums {
    void CheckSumCombine(unsigned int& sum, const HullType::Slot& slot) {
        TraceLogger() << "CheckSumCombine(Slot): " << typeid(slot).name();
        CheckSumCombine(sum, slot.x);
        CheckSumCombine(sum, slot.y);
        CheckSumCombine(sum, slot.type);
    }
}

////////////////////////////////////////////////
// Free Functions                             //
////////////////////////////////////////////////
const PartTypeManager& GetPartTypeManager()
{ return PartTypeManager::GetPartTypeManager(); }

const PartType* GetPartType(const std::string& name)
{ return GetPartTypeManager().GetPartType(name); }

const HullTypeManager& GetHullTypeManager()
{ return HullTypeManager::GetHullTypeManager(); }

const HullType* GetHullType(const std::string& name)
{ return GetHullTypeManager().GetHullType(name); }

const ShipDesign* GetShipDesign(int ship_design_id)
{ return GetUniverse().GetShipDesign(ship_design_id); }


/////////////////////////////////////
// PartTypeManager                 //
/////////////////////////////////////
// static
PartTypeManager* PartTypeManager::s_instance = nullptr;

PartTypeManager::PartTypeManager() {
    if (s_instance)
        throw std::runtime_error("Attempted to create more than one PartTypeManager.");

    ScopedTimer timer("PartTypeManager Init", true, std::chrono::milliseconds(1));

    try {
        parse::ship_parts(m_parts);
    } catch (const std::exception& e) {
        ErrorLogger() << "Failed parsing ship parts: error: " << e.what();
        throw;
    }

    TraceLogger() << "Part Types:";
    for (const auto& entry : m_parts) {
        const PartType* p = entry.second;
        TraceLogger() << " ... " << p->Name() << " class: " << p->Class();
    }

    // Only update the global pointer on sucessful construction.
    s_instance = this;

    DebugLogger() << "PartTypeManager checksum: " << GetCheckSum();
}

PartTypeManager::~PartTypeManager() {
    for (auto& entry : m_parts) {
        delete entry.second;
    }
}

const PartType* PartTypeManager::GetPartType(const std::string& name) const {
    auto it = m_parts.find(name);
    return it != m_parts.end() ? it->second : nullptr;
}

const PartTypeManager& PartTypeManager::GetPartTypeManager() {
    static PartTypeManager manager;
    return manager;
}

PartTypeManager::iterator PartTypeManager::begin() const
{ return m_parts.begin(); }

PartTypeManager::iterator PartTypeManager::end() const
{ return m_parts.end(); }

unsigned int PartTypeManager::GetCheckSum() const {
    unsigned int retval{0};
    for (auto const& name_part_pair : m_parts)
        CheckSums::CheckSumCombine(retval, name_part_pair);
    CheckSums::CheckSumCombine(retval, m_parts.size());

    return retval;
}


////////////////////////////////////////////////
// PartType
////////////////////////////////////////////////

PartType::PartType() :
    m_name("invalid part type"),
    m_description("indescribable"),
    m_class(INVALID_SHIP_PART_CLASS),
    m_capacity(0.0f),
    m_secondary_stat(1.0f),
    m_tertiary_stat(0.0f),
    m_production_cost(0),
    m_production_time(0),
    m_producible(false),
    m_mountable_slot_types(),
    m_tags(),
    m_production_meter_consumption(),
    m_production_special_consumption(),
    m_location(0),
    m_exclusions(),
    m_effects(),
    m_icon(),
    m_add_standard_capacity_effect(false)
{}

PartType::PartType(ShipPartClass part_class, double capacity, double stat2, double stat3,
                   const CommonParams& common_params, const MoreCommonParams& more_common_params,
                   std::vector<ShipSlotType> mountable_slot_types,
                   const std::string& icon, bool add_standard_capacity_effect) :
    m_name(more_common_params.name),
    m_description(more_common_params.description),
    m_class(part_class),
    m_capacity(capacity),
    m_secondary_stat(stat2),
    m_tertiary_stat(stat3),
    m_production_cost(common_params.production_cost),
    m_production_time(common_params.production_time),
    m_producible(common_params.producible),
    m_mountable_slot_types(mountable_slot_types),
    m_tags(),
    m_production_meter_consumption(common_params.production_meter_consumption),
    m_production_special_consumption(common_params.production_special_consumption),
    m_location(common_params.location),
    m_exclusions(more_common_params.exclusions),
    m_effects(),
    m_icon(icon),
    m_add_standard_capacity_effect(add_standard_capacity_effect)
{
    //TraceLogger() << "part type: " << m_name << " producible: " << m_producible << std::endl;
    Init(common_params.effects);
    for (const std::string& tag : common_params.tags)
        m_tags.insert(boost::to_upper_copy<std::string>(tag));
}

void PartType::Init(const std::vector<std::shared_ptr<Effect::EffectsGroup>>& effects) {
    if ((m_capacity != 0 || m_secondary_stat != 0 || m_tertiary_stat != 0) && m_add_standard_capacity_effect) {
        switch (m_class) {
        case PC_COLONY:
        case PC_TROOPS:
            m_effects.push_back(IncreaseMeter(METER_CAPACITY,       m_name, m_capacity, false));
            break;
        case PC_FIGHTER_HANGAR: {   // capacity indicates how many fighters are stored in this type of part (combined for all copies of the part)
            m_effects.push_back(IncreaseMeter(METER_MAX_CAPACITY,       m_name, m_capacity, true));         // stacking capacities allowed for this part, so each part contributes to the total capacity
            m_effects.push_back(IncreaseMeter(METER_MAX_SECONDARY_STAT, m_name, m_secondary_stat, false));  // stacking damage not allowed, as damage per fighter shot should be the same regardless of number of this part present
            m_effects.push_back(IncreaseMeter(METER_MAX_TERTIARY_STAT,  m_name, m_tertiary_stat, false));   // stacking noisiness not allowed, as stealth loss per figher launch should be the same regardless of number of this part present
            break;
        }
        case PC_FIGHTER_BAY:        // capacity indicates how many fighters each instance of the part can launch per combat bout...
        case PC_DIRECT_WEAPON: {    // capacity indicates weapon damage per shot
            m_effects.push_back(IncreaseMeter(METER_MAX_CAPACITY,       m_name, m_capacity, false));
            m_effects.push_back(IncreaseMeter(METER_MAX_SECONDARY_STAT, m_name, m_secondary_stat, false));
            m_effects.push_back(IncreaseMeter(METER_MAX_TERTIARY_STAT,  m_name, m_tertiary_stat, false));   // stacking noisiness not allowed, as stealth loss per shot should be the same regardless of number of this part present
            break;
        }
        case PC_SHIELD:
            m_effects.push_back(IncreaseMeter(METER_MAX_SHIELD,     m_capacity));
            break;
        case PC_DETECTION:
            m_effects.push_back(IncreaseMeter(METER_DETECTION,      m_capacity));
            break;
        case PC_STEALTH:
            m_effects.push_back(IncreaseMeter(METER_STEALTH,        m_capacity));
            break;
        case PC_FUEL:
            m_effects.push_back(IncreaseMeter(METER_MAX_FUEL,       m_capacity));
            break;
        case PC_ARMOUR:
            m_effects.push_back(IncreaseMeter(METER_MAX_STRUCTURE,  m_capacity,     "RULE_SHIP_STRUCTURE_FACTOR"));
            break;
        case PC_SPEED:
            m_effects.push_back(IncreaseMeter(METER_SPEED,          m_capacity,     "RULE_SHIP_SPEED_FACTOR"));
            break;
        case PC_RESEARCH:
            m_effects.push_back(IncreaseMeter(METER_TARGET_RESEARCH,m_capacity));
            break;
        case PC_INDUSTRY:
            m_effects.push_back(IncreaseMeter(METER_TARGET_INDUSTRY,m_capacity));
            break;
        case PC_TRADE:
            m_effects.push_back(IncreaseMeter(METER_TARGET_TRADE,   m_capacity));
            break;
        default:
            break;
        }
    }

    for (auto& effect : effects) {
        effect->SetTopLevelContent(m_name);
        m_effects.push_back(effect);
    }
}

PartType::~PartType()
{ delete m_location; }

float PartType::Capacity() const {
    switch (m_class) {
    case PC_ARMOUR:
        return m_capacity * GetGameRules().Get<double>("RULE_SHIP_STRUCTURE_FACTOR");
        break;
    case PC_SPEED:
        return m_capacity * GetGameRules().Get<double>("RULE_SHIP_SPEED_FACTOR");
        break;
    default:
        return m_capacity;
    }
}

float PartType::SecondaryStat() const
{ return m_secondary_stat; }

std::string PartType::CapacityDescription() const {
    std::string desc_string;
    float main_stat = Capacity();
    float sdry_stat = SecondaryStat();

    switch (m_class) {
    case PC_FUEL:
    case PC_TROOPS:
    case PC_COLONY:
    case PC_FIGHTER_BAY:
        desc_string += str(FlexibleFormat(UserString("PART_DESC_CAPACITY")) % main_stat);
        break;
    case PC_DIRECT_WEAPON:
        desc_string += str(FlexibleFormat(UserString("PART_DESC_DIRECT_FIRE_STATS")) % main_stat % sdry_stat);
        break;
    case PC_FIGHTER_HANGAR:
        desc_string += str(FlexibleFormat(UserString("PART_DESC_HANGAR_STATS")) % main_stat % sdry_stat);
        break;
    case PC_SHIELD:
        desc_string = str(FlexibleFormat(UserString("PART_DESC_SHIELD_STRENGTH")) % main_stat);
        break;
    case PC_DETECTION:
        desc_string = str(FlexibleFormat(UserString("PART_DESC_DETECTION")) % main_stat);
        break;
    default:
        desc_string = str(FlexibleFormat(UserString("PART_DESC_STRENGTH")) % main_stat);
        break;
    }
    return desc_string;
}

bool PartType::CanMountInSlotType(ShipSlotType slot_type) const {
    if (INVALID_SHIP_SLOT_TYPE == slot_type)
        return false;
    for (ShipSlotType mountable_slot_type : m_mountable_slot_types)
        if (mountable_slot_type == slot_type)
            return true;
    return false;
}

bool PartType::ProductionCostTimeLocationInvariant() const {
    if (GetGameRules().Get<bool>("RULE_CHEAP_AND_FAST_SHIP_PRODUCTION"))
        return true;
    if (m_production_cost && !m_production_cost->TargetInvariant())
        return false;
    if (m_production_time && !m_production_time->TargetInvariant())
        return false;
    return true;
}

float PartType::ProductionCost(int empire_id, int location_id) const {
    if (GetGameRules().Get<bool>("RULE_CHEAP_AND_FAST_SHIP_PRODUCTION") || !m_production_cost) {
        return 1.0f;
    } else {
        if (m_production_cost->ConstantExpr())
            return static_cast<float>(m_production_cost->Eval());
        else if (m_production_cost->SourceInvariant() && m_production_cost->TargetInvariant())
            return static_cast<float>(m_production_cost->Eval());

        const auto arbitrary_large_number = 999999.9f;

        auto location = GetUniverseObject(location_id);
        if (!location && !m_production_cost->TargetInvariant())
            return arbitrary_large_number;

        auto source = Empires().GetSource(empire_id);
        if (!source && !m_production_cost->SourceInvariant())
            return arbitrary_large_number;

        ScriptingContext context(source, location);

        return static_cast<float>(m_production_cost->Eval(context));
    }
}

int PartType::ProductionTime(int empire_id, int location_id) const {
    const auto arbitrary_large_number = 9999;

    if (GetGameRules().Get<bool>("RULE_CHEAP_AND_FAST_SHIP_PRODUCTION") || !m_production_time) {
        return 1;
    } else {
        if (m_production_time->ConstantExpr())
            return m_production_time->Eval();
        else if (m_production_time->SourceInvariant() && m_production_time->TargetInvariant())
            return m_production_time->Eval();

        auto location = GetUniverseObject(location_id);
        if (!location && !m_production_time->TargetInvariant())
            return arbitrary_large_number;

        auto source = Empires().GetSource(empire_id);
        if (!source && !m_production_time->SourceInvariant())
            return arbitrary_large_number;

        ScriptingContext context(source, location);

        return m_production_time->Eval(context);
    }
}

unsigned int PartType::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, m_name);
    CheckSums::CheckSumCombine(retval, m_description);
    CheckSums::CheckSumCombine(retval, m_class);
    CheckSums::CheckSumCombine(retval, m_capacity);
    CheckSums::CheckSumCombine(retval, m_secondary_stat);
    CheckSums::CheckSumCombine(retval, m_production_cost);
    CheckSums::CheckSumCombine(retval, m_production_time);
    CheckSums::CheckSumCombine(retval, m_producible);
    CheckSums::CheckSumCombine(retval, m_mountable_slot_types);
    CheckSums::CheckSumCombine(retval, m_tags);
    CheckSums::CheckSumCombine(retval, m_production_meter_consumption);
    CheckSums::CheckSumCombine(retval, m_production_special_consumption);
    CheckSums::CheckSumCombine(retval, m_location);
    CheckSums::CheckSumCombine(retval, m_exclusions);
    CheckSums::CheckSumCombine(retval, m_effects);
    CheckSums::CheckSumCombine(retval, m_icon);
    CheckSums::CheckSumCombine(retval, m_add_standard_capacity_effect);

    return retval;
}


////////////////////////////////////////////////
// HullType
////////////////////////////////////////////////
HullType::Slot::Slot() :
    type(INVALID_SHIP_SLOT_TYPE), x(0.5), y(0.5)
{}

void HullType::Init(const std::vector<std::shared_ptr<Effect::EffectsGroup>>& effects) {
    if (m_fuel != 0)
        m_effects.push_back(IncreaseMeter(METER_MAX_FUEL,       m_fuel));
    if (m_stealth != 0)
        m_effects.push_back(IncreaseMeter(METER_STEALTH,        m_stealth));
    if (m_structure != 0)
        m_effects.push_back(IncreaseMeter(METER_MAX_STRUCTURE,  m_structure,    "RULE_SHIP_STRUCTURE_FACTOR"));
    if (m_speed != 0)
        m_effects.push_back(IncreaseMeter(METER_SPEED,          m_speed,        "RULE_SHIP_SPEED_FACTOR"));

    for (auto& effect : effects) {
        effect->SetTopLevelContent(m_name);
        m_effects.push_back(effect);
    }
}

HullType::~HullType()
{ delete m_location; }

float HullType::Speed() const
{ return m_speed * GetGameRules().Get<double>("RULE_SHIP_SPEED_FACTOR"); }

float HullType::Structure() const
{ return m_structure * GetGameRules().Get<double>("RULE_SHIP_STRUCTURE_FACTOR"); }

unsigned int HullType::NumSlots(ShipSlotType slot_type) const {
    unsigned int count = 0;
    for (const Slot& slot : m_slots)
        if (slot.type == slot_type)
            ++count;
    return count;
}

// HullType:: and PartType::ProductionCost and ProductionTime are almost identical.
// Chances are, the same is true of buildings and techs as well.
// TODO: Eliminate duplication
bool HullType::ProductionCostTimeLocationInvariant() const {
    if (GetGameRules().Get<bool>("RULE_CHEAP_AND_FAST_SHIP_PRODUCTION"))
        return true;
    if (m_production_cost && !m_production_cost->LocalCandidateInvariant())
        return false;
    if (m_production_time && !m_production_time->LocalCandidateInvariant())
        return false;
    return true;
}

float HullType::ProductionCost(int empire_id, int location_id) const {
    if (GetGameRules().Get<bool>("RULE_CHEAP_AND_FAST_SHIP_PRODUCTION") || !m_production_cost) {
        return 1.0f;
    } else {
        if (m_production_cost->ConstantExpr())
            return static_cast<float>(m_production_cost->Eval());
        else if (m_production_cost->SourceInvariant() && m_production_cost->TargetInvariant())
            return static_cast<float>(m_production_cost->Eval());

        const auto arbitrary_large_number = 999999.9f;

        auto location = GetUniverseObject(location_id);
        if (!location && !m_production_cost->TargetInvariant())
            return arbitrary_large_number;

        auto source = Empires().GetSource(empire_id);
        if (!source && !m_production_cost->SourceInvariant())
            return arbitrary_large_number;

        ScriptingContext context(source, location);

        return static_cast<float>(m_production_cost->Eval(context));
    }
}

int HullType::ProductionTime(int empire_id, int location_id) const {
    if (GetGameRules().Get<bool>("RULE_CHEAP_AND_FAST_SHIP_PRODUCTION") || !m_production_time) {
        return 1;
    } else {
        if (m_production_time->ConstantExpr())
            return m_production_time->Eval();
        else if (m_production_time->SourceInvariant() && m_production_time->TargetInvariant())
            return m_production_time->Eval();

        const auto arbitrary_large_number = 999999;

        auto location = GetUniverseObject(location_id);
        if (!location && !m_production_time->TargetInvariant())
            return arbitrary_large_number;

        auto source = Empires().GetSource(empire_id);
        if (!source && !m_production_time->SourceInvariant())
            return arbitrary_large_number;

        ScriptingContext context(source, location);

        return m_production_time->Eval(context);
    }
}

unsigned int HullType::GetCheckSum() const {
    unsigned int retval{0};

    //// test
    //std::map<unsigned long int, std::shared_ptr<Blah>> int_pBlah_map{
    //    {103U, std::make_shared<Blah>()}, {0, nullptr}};
    //CheckSumCombine(retval, int_pBlah_map);
    //std::map<MeterType, std::string> metertype_string_map{{METER_INDUSTRY, "STRING!"}};
    //CheckSumCombine(retval, metertype_string_map);
    //return retval;
    //// end test

    CheckSums::CheckSumCombine(retval, m_name);
    CheckSums::CheckSumCombine(retval, m_description);
    CheckSums::CheckSumCombine(retval, m_speed);
    CheckSums::CheckSumCombine(retval, m_fuel);
    CheckSums::CheckSumCombine(retval, m_stealth);
    CheckSums::CheckSumCombine(retval, m_structure);
    CheckSums::CheckSumCombine(retval, m_production_cost);
    CheckSums::CheckSumCombine(retval, m_production_time);
    CheckSums::CheckSumCombine(retval, m_producible);
    CheckSums::CheckSumCombine(retval, m_slots);
    CheckSums::CheckSumCombine(retval, m_tags);
    CheckSums::CheckSumCombine(retval, m_production_meter_consumption);
    CheckSums::CheckSumCombine(retval, m_production_special_consumption);
    CheckSums::CheckSumCombine(retval, m_location);
    CheckSums::CheckSumCombine(retval, m_exclusions);
    CheckSums::CheckSumCombine(retval, m_effects);
    CheckSums::CheckSumCombine(retval, m_graphic);
    CheckSums::CheckSumCombine(retval, m_icon);

    return retval;
}


/////////////////////////////////////
// HullTypeManager                 //
/////////////////////////////////////
// static
HullTypeManager* HullTypeManager::s_instance = nullptr;

HullTypeManager::HullTypeManager() {
    if (s_instance)
        throw std::runtime_error("Attempted to create more than one HullTypeManager.");

    ScopedTimer timer("HullTypeManager Init", true, std::chrono::milliseconds(1));

    try {
        parse::ship_hulls(m_hulls);
    } catch (const std::exception& e) {
        ErrorLogger() << "Failed parsing ship hulls: error: " << e.what();
        throw;
    }

    TraceLogger() << "Hull Types:";
    for (const auto& entry : m_hulls) {
        const HullType* h = entry.second;
        TraceLogger() << " ... " << h->Name();
    }

    if (m_hulls.empty())
        ErrorLogger() << "HullTypeManager expects at least one hull type.  All ship design construction will fail.";

    // Only update the global pointer on sucessful construction.
    s_instance = this;

    DebugLogger() << "HullTypeManager checksum: " << GetCheckSum();
}

HullTypeManager::~HullTypeManager() {
    for (auto& entry : m_hulls) {
        delete entry.second;
    }
}

const HullType* HullTypeManager::GetHullType(const std::string& name) const {
    auto it = m_hulls.find(name);
    return it != m_hulls.end() ? it->second : nullptr;
}

const HullTypeManager& HullTypeManager::GetHullTypeManager() {
    static HullTypeManager manager;
    return manager;
}

HullTypeManager::iterator HullTypeManager::begin() const
{ return m_hulls.begin(); }

HullTypeManager::iterator HullTypeManager::end() const
{ return m_hulls.end(); }

unsigned int HullTypeManager::GetCheckSum() const {
    unsigned int retval{0};
    for (auto const& name_hull_pair : m_hulls)
        CheckSums::CheckSumCombine(retval, name_hull_pair);
    CheckSums::CheckSumCombine(retval, m_hulls.size());

    return retval;
}

////////////////////////////////////////////////
// ShipDesign
////////////////////////////////////////////////
ShipDesign::ShipDesign() :
    m_name(),
    m_description(),
    m_uuid(boost::uuids::nil_generator()()),
    m_designed_on_turn(UniverseObject::INVALID_OBJECT_AGE),
    m_designed_by_empire(ALL_EMPIRES),
    m_hull(),
    m_parts(),
    m_is_monster(false),
    m_icon(),
    m_3D_model(),
    m_name_desc_in_stringtable(false)
{}

ShipDesign::ShipDesign(const boost::optional<std::invalid_argument>& should_throw,
                       const std::string& name, const std::string& description,
                       int designed_on_turn, int designed_by_empire, const std::string& hull,
                       const std::vector<std::string>& parts,
                       const std::string& icon, const std::string& model,
                       bool name_desc_in_stringtable, bool monster,
                       const boost::uuids::uuid& uuid /*= boost::uuids::nil_uuid()*/) :
    m_name(name),
    m_description(description),
    m_uuid(uuid),
    m_designed_on_turn(designed_on_turn),
    m_designed_by_empire(designed_by_empire),
    m_hull(hull),
    m_parts(parts),
    m_is_monster(monster),
    m_icon(icon),
    m_3D_model(model),
    m_name_desc_in_stringtable(name_desc_in_stringtable)
{
    // Either force a valid design and log about it or just throw std::invalid_argument
    ForceValidDesignOrThrow(should_throw, !should_throw);
    BuildStatCaches();
}

ShipDesign::ShipDesign(const std::string& name, const std::string& description,
                       int designed_on_turn, int designed_by_empire, const std::string& hull,
                       const std::vector<std::string>& parts,
                       const std::string& icon, const std::string& model,
                       bool name_desc_in_stringtable, bool monster,
                       const boost::uuids::uuid& uuid /*= boost::uuids::nil_uuid()*/) :
    ShipDesign(boost::none, name, description, designed_on_turn, designed_by_empire, hull, parts,
               icon, model, name_desc_in_stringtable, monster, uuid)
{}

const std::string& ShipDesign::Name(bool stringtable_lookup /* = true */) const {
    if (m_name_desc_in_stringtable && stringtable_lookup)
        return UserString(m_name);
    else
        return m_name;
}

void ShipDesign::SetName(const std::string& name) {
    if (m_name != "") {
        m_name = name;
    }
}

void ShipDesign::SetUUID(const boost::uuids::uuid& uuid)
{ m_uuid = uuid; }

const std::string& ShipDesign::Description(bool stringtable_lookup /* = true */) const {
    if (m_name_desc_in_stringtable && stringtable_lookup)
        return UserString(m_description);
    else
        return m_description;
}

void ShipDesign::SetDescription(const std::string& description) {
    if (m_description != "") {
        m_description = description;
    }
}

bool ShipDesign::ProductionCostTimeLocationInvariant() const {
    if (GetGameRules().Get<bool>("RULE_CHEAP_AND_FAST_SHIP_PRODUCTION"))
        return true;
    // as seen in ShipDesign::ProductionCost, the location is passed as the
    // local candidate in the ScriptingContext

    // check hull and all parts
    if (const HullType* hull = GetHullType(m_hull))
        if (!hull->ProductionCostTimeLocationInvariant())
            return false;
    for (const std::string& part_name : m_parts)
        if (const PartType* part = GetPartType(part_name))
            if (!part->ProductionCostTimeLocationInvariant())
                return false;

    // if hull and all parts are invariant, so is whole design
    return true;
}

float ShipDesign::ProductionCost(int empire_id, int location_id) const {
    if (GetGameRules().Get<bool>("RULE_CHEAP_AND_FAST_SHIP_PRODUCTION")) {
        return 1.0f;
    } else {
        float cost_accumulator = 0.0f;
        if (const HullType* hull = GetHullType(m_hull))
            cost_accumulator += hull->ProductionCost(empire_id, location_id);
        for (const std::string& part_name : m_parts)
            if (const PartType* part = GetPartType(part_name))
                cost_accumulator += part->ProductionCost(empire_id, location_id);
        return std::max(0.0f, cost_accumulator);
    }
}

float ShipDesign::PerTurnCost(int empire_id, int location_id) const
{ return ProductionCost(empire_id, location_id) / std::max(1, ProductionTime(empire_id, location_id)); }

int ShipDesign::ProductionTime(int empire_id, int location_id) const {
    if (GetGameRules().Get<bool>("RULE_CHEAP_AND_FAST_SHIP_PRODUCTION")) {
        return 1;
    } else {
        int time_accumulator = 1;
        if (const HullType* hull = GetHullType(m_hull))
            time_accumulator = std::max(time_accumulator, hull->ProductionTime(empire_id, location_id));
        for (const std::string& part_name : m_parts)
            if (const PartType* part = GetPartType(part_name))
                time_accumulator = std::max(time_accumulator, part->ProductionTime(empire_id, location_id));
        return std::max(1, time_accumulator);
    }
}

bool ShipDesign::CanColonize() const {
    for (const std::string& part_name : m_parts) {
        if (part_name.empty())
            continue;
        if (const PartType* part = GetPartType(part_name))
            if (part->Class() == PC_COLONY)
                return true;
    }
    return false;
}

float ShipDesign::Defense() const {
    // accumulate defense from defensive parts in design.
    float total_defense = 0.0f;
    const PartTypeManager& part_manager = GetPartTypeManager();
    for (const std::string& part_name : Parts()) {
        const PartType* part = part_manager.GetPartType(part_name);
        if (part && (part->Class() == PC_SHIELD || part->Class() == PC_ARMOUR))
            total_defense += part->Capacity();
    }
    return total_defense;
}

float ShipDesign::Attack() const {
    // total damage against a target with the no shield.
    return AdjustedAttack(0.0f);
}

float ShipDesign::AdjustedAttack(float shield) const {
    // total damage against a target with the given shield (damage reduction)
    // assuming full load of fighters that are not destroyed during the battle
    int available_fighters = 0;
    int fighter_launch_capacity = 0;
    float fighter_damage = 0.0f;
    float direct_attack = 0.0f;

    for (const std::string& part_name : m_parts) {
        const PartType* part = GetPartType(part_name);
        if (!part)
            continue;
        ShipPartClass part_class = part->Class();

        // direct weapon and fighter-related parts all handled differently...
        if (part_class == PC_DIRECT_WEAPON) {
            float part_attack = part->Capacity();
            if (part_attack > shield)
                direct_attack += (part_attack - shield)*part->SecondaryStat();  // here, secondary stat is number of shots per round
        } else if (part_class == PC_FIGHTER_HANGAR) {
            available_fighters = part->Capacity();                              // stacked meter
        } else if (part_class == PC_FIGHTER_BAY) {
            fighter_launch_capacity += part->Capacity();
            fighter_damage = part->SecondaryStat();                             // here, secondary stat is fighter damage per shot
        }
    }

    int fighter_shots = std::min(available_fighters, fighter_launch_capacity);  // how many fighters launched in bout 1
    available_fighters -= fighter_shots;
    int launched_fighters = fighter_shots;
    int num_bouts = GetGameRules().Get<int>("RULE_NUM_COMBAT_ROUNDS");
    int remaining_bouts = num_bouts - 2;  // no attack for first round, second round already added
    while (remaining_bouts > 0) {
        int fighters_launched_this_bout = std::min(available_fighters, fighter_launch_capacity);
        available_fighters -= fighters_launched_this_bout;
        launched_fighters += fighters_launched_this_bout;
        fighter_shots += launched_fighters;
        --remaining_bouts;
    }

    // how much damage does a fighter shot do?
    fighter_damage = std::max(0.0f, fighter_damage);

    return direct_attack + fighter_shots*fighter_damage/num_bouts;   // divide by bouts because fighter calculation is for a full combat, but direct firefor one attack
}

std::vector<std::string> ShipDesign::Parts(ShipSlotType slot_type) const {
    std::vector<std::string> retval;

    const HullType* hull = GetHull();
    if (!hull) {
        ErrorLogger() << "Design hull not found: " << m_hull;
        return retval;
    }
    const std::vector<HullType::Slot>& slots = hull->Slots();

    if (m_parts.empty())
        return retval;

    // add to output vector each part that is in a slot of the indicated ShipSlotType
    for (unsigned int i = 0; i < m_parts.size(); ++i)
        if (slots[i].type == slot_type)
            retval.push_back(m_parts[i]);

    return retval;
}

std::vector<std::string> ShipDesign::Weapons() const {
    std::vector<std::string> retval;
    retval.reserve(m_parts.size());
    for (const std::string& part_name : m_parts) {
        const PartType* part = GetPartType(part_name);
        if (!part)
            continue;
        ShipPartClass part_class = part->Class();
        if (part_class == PC_DIRECT_WEAPON || part_class == PC_FIGHTER_BAY)
        { retval.push_back(part_name); }
    }
    return retval;
}

bool ShipDesign::ProductionLocation(int empire_id, int location_id) const {
    Empire* empire = GetEmpire(empire_id);
    if (!empire) {
        DebugLogger() << "ShipDesign::ProductionLocation: Unable to get pointer to empire " << empire_id;
        return false;
    }

    // must own the production location...
    auto location = GetUniverseObject(location_id);
    if (!location) {
        WarnLogger() << "ShipDesign::ProductionLocation unable to get location object with id " << location_id;
        return false;
    }
    if (!location->OwnedBy(empire_id))
        return false;

    auto planet = std::dynamic_pointer_cast<const Planet>(location);
    std::shared_ptr<const Ship> ship;
    if (!planet)
        ship = std::dynamic_pointer_cast<const Ship>(location);
    if (!planet && !ship)
        return false;

    // ships can only be produced by species that are not planetbound
    const std::string& species_name = planet ? planet->SpeciesName() : (ship ? ship->SpeciesName() : EMPTY_STRING);
    if (species_name.empty())
        return false;
    const Species* species = GetSpecies(species_name);
    if (!species)
        return false;

    if (!species->CanProduceShips())
        return false;
    // also, species that can't colonize can't produce colony ships
    if (this->CanColonize() && !species->CanColonize())
        return false;

    // apply hull location conditions to potential location
    const HullType* hull = GetHull();
    if (!hull) {
        ErrorLogger() << "ShipDesign::ProductionLocation  ShipDesign couldn't get its own hull with name " << m_hull;
        return false;
    }
    // evaluate using location as the source, as it should be an object owned by this empire.
    ScriptingContext location_as_source_context(location);
    if (!hull->Location()->Eval(location_as_source_context, location))
        return false;

    // apply external and internal parts' location conditions to potential location
    for (const std::string& part_name : m_parts) {
        if (part_name.empty())
            continue;       // empty slots don't limit build location

        const PartType* part = GetPartType(part_name);
        if (!part) {
            ErrorLogger() << "ShipDesign::ProductionLocation  ShipDesign couldn't get part with name " << part_name;
            return false;
        }
        if (!part->Location()->Eval(location_as_source_context, location))
            return false;
    }
    // location matched all hull and part conditions, so is a valid build location
    return true;
}

void ShipDesign::SetID(int id)
{ m_id = id; }

bool ShipDesign::ValidDesign(const std::string& hull, const std::vector<std::string>& parts_in) {
    auto parts = parts_in;
    return !MaybeInvalidDesign(hull, parts, true);
}

boost::optional<std::pair<std::string, std::vector<std::string>>>
ShipDesign::MaybeInvalidDesign(const std::string& hull_in,
                               std::vector<std::string>& parts_in,
                               bool produce_log)
{
    bool is_valid = true;

    auto hull = hull_in;
    auto parts = parts_in;

    // ensure hull type exists
    const HullType* input_hull_type = GetHullTypeManager().GetHullType(hull);
    HullType* fallback_hull_type;
    if (!input_hull_type) {
        is_valid = false;
        if (produce_log)
            WarnLogger() << "Invalid ShipDesign hull not found: " << hull;

        const auto hull_it = GetHullTypeManager().begin();
        if (hull_it != GetHullTypeManager().end()) {
            std::tie(hull, fallback_hull_type) = *hull_it;
            if (produce_log)
                WarnLogger() << "Invalid ShipDesign hull falling back to: " << hull;
        } else {
            if (produce_log)
                ErrorLogger() << "Invalid ShipDesign no available hulls ";
            hull = "";
            parts.clear();
            return std::make_pair(hull, parts);
        }
    }

    const auto hull_type = input_hull_type ? input_hull_type : fallback_hull_type;


    // ensure hull type has at least enough slots for passed parts
    if (parts.size() > hull_type->NumSlots()) {
        is_valid = false;
        if (produce_log)
            WarnLogger() << "Invalid ShipDesign given " << parts.size() << " parts for hull with "
                         << hull_type->NumSlots() << " slots.  Truncating last "
                         << (parts.size() - hull_type->NumSlots()) << " parts.";
    }

    // If parts is smaller than the full hull size pad it and the incoming parts
    if (parts.size() < hull_type->NumSlots())
        parts_in.resize(hull_type->NumSlots(), "");

    // Truncate or pad with "" parts.
    parts.resize(hull_type->NumSlots(), "");

    const auto& slots = hull_type->Slots();

    // check hull exclusions against all parts...
    const auto& hull_exclusions = hull_type->Exclusions();
    for (auto& part_name : parts) {
        if (part_name.empty())
            continue;
        if (hull_exclusions.count(part_name)) {
            is_valid = false;
            if (produce_log)
                WarnLogger() << "Invalid ShipDesign part \"" << part_name << "\" is excluded by \""
                             << hull_type->Name() << "\". Removing \"" << part_name <<"\"";
            part_name.clear();
        }
    }

    // check part exclusions against other parts and hull
    std::unordered_set<std::string> already_seen_component_names;
    already_seen_component_names.insert(hull);

    for (std::size_t ii = 0; ii < parts.size(); ++ii) {
        auto& part_name =  parts[ii];

        // Ignore empty slots which are valid.
        if (part_name.empty())
            continue;

        // Remove parts that don't exist
        const auto part_type = GetPartType(part_name);
        if (!part_type) {
            if (produce_log)
                WarnLogger() << "Invalid ShipDesign part \"" << part_name << "\" not found"
                             << ". Removing \"" << part_name <<"\"";
            is_valid = false;
            part_name.clear();
            continue;
        }

        for (const auto& excluded : part_type->Exclusions()) {
            if (already_seen_component_names.count(excluded)) {
                is_valid = false;
                if (produce_log)
                    WarnLogger() << "Invalid ShipDesign part " << part_name << " conflicts with \""
                                 << excluded << "\". Removing \"" << part_name <<"\"";
                part_name.clear();
                continue;
            }
        }

        // verify part can mount in indicated slot
        const ShipSlotType& slot_type = slots[ii].type;

        if (!part_type->CanMountInSlotType(slot_type)) {
            if (produce_log)
                DebugLogger() << "Invalid ShipDesign part \"" << part_name << "\" can't be mounted in "
                              << boost::lexical_cast<std::string>(slot_type) << " slot"
                              << ". Removing \"" << part_name <<"\"";
            is_valid = false;
            part_name.clear();
            continue;
        }

        if (!part_name.empty())
            already_seen_component_names.insert(part_name);
    }

    if (is_valid)
        return boost::none;
    else
        return std::make_pair(hull, parts);
}

void ShipDesign::ForceValidDesignOrThrow(const boost::optional<std::invalid_argument>& should_throw,
                                         bool  produce_log)
{
    auto force_valid = MaybeInvalidDesign(m_hull, m_parts, produce_log);
    if (!force_valid)
        return;

    if (!produce_log && should_throw)
        throw std::invalid_argument("ShipDesign: Bad hull or parts");

    std::stringstream ss;

    bool no_hull_available = force_valid->first.empty();
    if (no_hull_available)
        ss << "ShipDesign has no valid hull and there are no other hulls available." << std::endl;

    ss << "Invalid ShipDesign:" << std::endl;
    ss << Dump() << std::endl;

    std::tie(m_hull, m_parts) = *force_valid;

    ss << "ShipDesign was made valid as:" << std::endl;
    ss << Dump() << std::endl;

    if (no_hull_available)
        ErrorLogger() << ss.str();
    else
        WarnLogger() << ss.str();

    if (should_throw)
        throw std::invalid_argument("ShipDesign: Bad hull or parts");
}

void ShipDesign::BuildStatCaches() {
    const HullType* hull = GetHullType(m_hull);
    if (!hull) {
        ErrorLogger() << "ShipDesign::BuildStatCaches couldn't get hull with name " << m_hull;
        return;
    }

    m_producible =      hull->Producible();
    m_detection =       hull->Detection();
    m_colony_capacity = hull->ColonyCapacity();
    m_troop_capacity =  hull->TroopCapacity();
    m_stealth =         hull->Stealth();
    m_fuel =            hull->Fuel();
    m_shields =         hull->Shields();
    m_structure =       hull->Structure();
    m_speed =           hull->Speed();

    for (const std::string& part_name : m_parts) {
        if (part_name.empty())
            continue;

        const PartType* part = GetPartType(part_name);
        if (!part) {
            ErrorLogger() << "ShipDesign::BuildStatCaches couldn't get part with name " << part_name;
            continue;
        }

        if (!part->Producible())
            m_producible = false;

        ShipPartClass part_class = part->Class();

        switch (part_class) {
        case PC_DIRECT_WEAPON:
            m_is_armed = true;
            break;
        case PC_FIGHTER_BAY:
        case PC_FIGHTER_HANGAR:
            m_has_fighters = true;
            break;
        case PC_COLONY:
            m_colony_capacity += part->Capacity();
            break;
        case PC_TROOPS:
            m_troop_capacity += part->Capacity();
            break;
        case PC_STEALTH:
            m_stealth += part->Capacity();
            break;
        case PC_SPEED:
            m_speed += part->Capacity();
            break;
        case PC_SHIELD:
            m_shields += part->Capacity();
            break;
        case PC_FUEL:
            m_fuel += part->Capacity();
            break;
        case PC_ARMOUR:
            m_structure += part->Capacity();
            break;
        case PC_DETECTION:
            m_detection += part->Capacity();
            break;
        case PC_BOMBARD:
            m_can_bombard = true;
            break;
        case PC_RESEARCH:
            m_research_generation += part->Capacity();
            break;
        case PC_INDUSTRY:
            m_industry_generation += part->Capacity();
            break;
        case PC_TRADE:
            m_trade_generation += part->Capacity();
            break;
        case PC_PRODUCTION_LOCATION:
            m_is_production_location = true;
            break;
        default:
            break;
        }

        m_num_part_types[part_name]++;
        if (part_class > INVALID_SHIP_PART_CLASS && part_class < NUM_SHIP_PART_CLASSES)
            m_num_part_classes[part_class]++;
    }
}

std::string ShipDesign::Dump() const {
    std::string retval = DumpIndent() + "ShipDesign\n";
    ++g_indent;
    retval += DumpIndent() + "name = \"" + m_name + "\"\n";
    retval += DumpIndent() + "uuid = \"" + boost::uuids::to_string(m_uuid) + "\"\n";
    retval += DumpIndent() + "description = \"" + m_description + "\"\n";

    if (!m_name_desc_in_stringtable)
        retval += DumpIndent() + "NoStringtableLookup\n";
    retval += DumpIndent() + "hull = \"" + m_hull + "\"\n";
    retval += DumpIndent() + "parts = ";
    if (m_parts.empty()) {
        retval += "[]\n";
    } else if (m_parts.size() == 1) {
        retval += "\"" + *m_parts.begin() + "\"\n";
    } else {
        retval += "[\n";
        ++g_indent;
        for (const std::string& part_name : m_parts) {
            retval += DumpIndent() + "\"" + part_name + "\"\n";
        }
        --g_indent;
        retval += DumpIndent() + "]\n";
    }
    if (!m_icon.empty())
        retval += DumpIndent() + "icon = \"" + m_icon + "\"\n";
    retval += DumpIndent() + "model = \"" + m_3D_model + "\"\n";
    --g_indent;
    return retval; 
}

unsigned int ShipDesign::GetCheckSum() const {
    unsigned int retval{0};
    CheckSums::CheckSumCombine(retval, m_id);
    CheckSums::CheckSumCombine(retval, m_uuid);
    CheckSums::CheckSumCombine(retval, m_name);
    CheckSums::CheckSumCombine(retval, m_description);
    CheckSums::CheckSumCombine(retval, m_designed_on_turn);
    CheckSums::CheckSumCombine(retval, m_designed_by_empire);
    CheckSums::CheckSumCombine(retval, m_hull);
    CheckSums::CheckSumCombine(retval, m_parts);
    CheckSums::CheckSumCombine(retval, m_is_monster);
    CheckSums::CheckSumCombine(retval, m_icon);
    CheckSums::CheckSumCombine(retval, m_3D_model);
    CheckSums::CheckSumCombine(retval, m_name_desc_in_stringtable);

    return retval;
}

bool operator ==(const ShipDesign& first, const ShipDesign& second) {
    if (first.Hull() != second.Hull())
        return false;

    std::map<std::string, int> first_parts;
    std::map<std::string, int> second_parts;

    for (const std::string& part_name : first.Parts())
    { ++first_parts[part_name]; }

    for (const std::string& part_name : second.Parts())
    { ++second_parts[part_name]; }

    return first_parts == second_parts;
}


/////////////////////////////////////
// PredefinedShipDesignManager     //
/////////////////////////////////////
// static(s)
PredefinedShipDesignManager* PredefinedShipDesignManager::s_instance = nullptr;

namespace {
    template <typename Map1, typename Map2, typename Ordering>
    void FillDesignsOrderingAndNameTables(const boost::filesystem::path& path,
                                          Map1& designs, Ordering& ordering, Map2& name_to_uuid)
    {
        name_to_uuid.clear();

        auto inconsistent_and_map_and_order_ships =
            LoadShipDesignsAndManifestOrderFromFileSystem(path);

        ordering = std::get<2>(inconsistent_and_map_and_order_ships);

        auto& disk_designs = std::get<1>(inconsistent_and_map_and_order_ships);

        for (auto& uuid_and_design : disk_designs) {
            auto& design = uuid_and_design.second.first;

            if (designs.count(design->UUID())) {
                ErrorLogger() << design->Name() << " ship design does not have a unique UUID for "
                              << "its type monster or pre-defined. "
                              << designs[design->UUID()]->Name() << " has the same UUID.";
                continue;
            }

            if (name_to_uuid.count(design->Name())) {
                ErrorLogger() << design->Name() << " ship design does not have a unique name for "
                              << "its type monster or pre-defined.";
                continue;
            }

            name_to_uuid.insert(std::make_pair(design->Name(), design->UUID()));
            designs[design->UUID()] = std::move(design);
        }
    }
}

PredefinedShipDesignManager::PredefinedShipDesignManager() {
    if (s_instance)
        throw std::runtime_error("Attempted to create more than one PredefinedShipDesignManager.");

    ScopedTimer timer("PredefinedShipDesignManager Init", true, std::chrono::milliseconds(1));

    DebugLogger() << "Initializing PredefinedShipDesignManager";

    m_designs.clear();

    FillDesignsOrderingAndNameTables(
        "scripting/ship_designs", m_designs, m_ship_ordering, m_name_to_ship_design);
    FillDesignsOrderingAndNameTables(
        "scripting/monster_designs", m_designs, m_monster_ordering, m_name_to_monster_design);

    // Make the monsters monstrous
    for (const auto& uuid : m_monster_ordering)
        m_designs[uuid]->SetMonster(true);

    TraceLogger() << "Predefined Ship Designs:";
    for (const auto& entry : m_designs)
        TraceLogger() << " ... " << entry.second->Name();

    // Only update the global pointer on sucessful construction.
    s_instance = this;

    DebugLogger() << "PredefinedShipDesignManager checksum: " << GetCheckSum();
}

namespace {
    void AddDesignToUniverse(std::unordered_map<std::string, int>& design_generic_ids,
                             const std::unique_ptr<ShipDesign>& design, bool monster)
    {
        if (!design)
            return;

        Universe& universe = GetUniverse();
        /* check if there already exists this same design in the universe. */
        for (auto it = universe.beginShipDesigns();
             it != universe.endShipDesigns(); ++it)
        {
            const ShipDesign* existing_design = it->second;
            if (!existing_design) {
                ErrorLogger() << "PredefinedShipDesignManager::AddShipDesignsToUniverse found an invalid design in the Universe";
                continue;
            }

            if (DesignsTheSame(*existing_design, *design)) {
                WarnLogger() << "AddShipDesignsToUniverse found an exact duplicate of ship design "
                             << design->Name() << "to be added, so is not re-adding it";
                design_generic_ids[design->Name(false)] = existing_design->ID();
                return; // design already added; don't need to do so again
            }
        }

        // duplicate design to add to Universe
        ShipDesign* copy = new ShipDesign(*design);

        bool success = universe.InsertShipDesign(copy);
        if (!success) {
            ErrorLogger() << "Empire::AddShipDesign Unable to add new design to universe";
            delete copy;
            return;
        }

        auto new_design_id = copy->ID();
        design_generic_ids[design->Name(false)] = new_design_id;
        TraceLogger() << "AddShipDesignsToUniverse added ship design "
                      << design->Name() << " to universe.";
    };
}

void PredefinedShipDesignManager::AddShipDesignsToUniverse() const {
    m_design_generic_ids.clear();

    for (const auto& uuid : m_ship_ordering)
        AddDesignToUniverse(m_design_generic_ids, m_designs.at(uuid), false);

    for (const auto& uuid : m_monster_ordering)
        AddDesignToUniverse(m_design_generic_ids, m_designs.at(uuid), true);
}

PredefinedShipDesignManager& PredefinedShipDesignManager::GetPredefinedShipDesignManager() {
    static PredefinedShipDesignManager manager;
    return manager;
}


std::vector<const ShipDesign*> PredefinedShipDesignManager::GetOrderedShipDesigns() const {
    std::vector<const ShipDesign*> retval;
    for (const auto& uuid : m_ship_ordering)
        retval.push_back(m_designs.at(uuid).get());
    return retval;
}

std::vector<const ShipDesign*> PredefinedShipDesignManager::GetOrderedMonsterDesigns() const {
    std::vector<const ShipDesign*> retval;
    for (const auto& uuid : m_monster_ordering)
        retval.push_back(m_designs.at(uuid).get());
    return retval;
}

int PredefinedShipDesignManager::GetDesignID(const std::string& name) const {
    const auto& it = m_design_generic_ids.find(name);
    if (it == m_design_generic_ids.end())
        return INVALID_DESIGN_ID;
    return it->second;
}

unsigned int PredefinedShipDesignManager::GetCheckSum() const {
    unsigned int retval{0};

    auto build_checksum = [&retval, this](const std::vector<boost::uuids::uuid>& ordering){
        for (auto const& uuid : ordering) {
            auto it = m_designs.find(uuid);
            if (it != m_designs.end())
                CheckSums::CheckSumCombine(retval, std::make_pair(it->second->Name(), *it->second));
        }
        CheckSums::CheckSumCombine(retval, ordering.size());
    };

    build_checksum(m_ship_ordering);
    build_checksum(m_monster_ordering);

    return retval;
}


///////////////////////////////////////////////////////////
// Free Functions                                        //
///////////////////////////////////////////////////////////
const PredefinedShipDesignManager& GetPredefinedShipDesignManager()
{ return PredefinedShipDesignManager::GetPredefinedShipDesignManager(); }

const ShipDesign* GetPredefinedShipDesign(const std::string& name)
{ return GetUniverse().GetGenericShipDesign(name); }

std::tuple<
    bool,
    std::unordered_map<boost::uuids::uuid,
                       std::pair<std::unique_ptr<ShipDesign>, boost::filesystem::path>,
                       boost::hash<boost::uuids::uuid>>,
    std::vector<boost::uuids::uuid>>
LoadShipDesignsAndManifestOrderFromFileSystem(const boost::filesystem::path& dir) {
    std::unordered_map<boost::uuids::uuid,
                       std::pair<std::unique_ptr<ShipDesign>,
                                 boost::filesystem::path>,
                       boost::hash<boost::uuids::uuid>>  saved_designs;
    std::vector<boost::uuids::uuid> disk_ordering;

    std::vector<std::pair<std::unique_ptr<ShipDesign>, boost::filesystem::path>> designs_and_paths;
    parse::ship_designs(dir, designs_and_paths, disk_ordering);

    for (auto&& design_and_path : designs_and_paths) {
        auto& design = design_and_path.first;

        // If the UUID is nil this is a legacy design that needs a new UUID
        if(design->UUID() == boost::uuids::uuid{{0}}) {
            design->SetUUID(boost::uuids::random_generator()());
            DebugLogger() << "Converted legacy ship design file by adding  UUID " << design->UUID()
                          << " for name " << design->Name();
        }

        // Make sure the design is an out of universe object
        // This should not be needed.
        if(design->ID() != INVALID_OBJECT_ID) {
            design->SetID(INVALID_OBJECT_ID);
            ErrorLogger() << "Loaded ship design has an id implying it is in an ObjectMap for UUID "
                          << design->UUID() << " for name " << design->Name();
        }

        if (!saved_designs.count(design->UUID())) {
            TraceLogger() << "Added saved design UUID " << design->UUID()
                          << " with name " << design->Name();
            saved_designs[design->UUID()] = std::move(design_and_path);
        } else {
            WarnLogger() << "Duplicate ship design UUID " << design->UUID()
                         << " found for ship design " << design->Name()
                         << " and " << saved_designs[design->UUID()].first->Name()
                         << " in " << dir;
        }
    }

    // Verify that all UUIDs in ordering exist
    std::vector<boost::uuids::uuid> ordering;
    bool ship_manifest_inconsistent = false;
    for (auto& uuid: disk_ordering) {
        // Skip the nil UUID.
        if(uuid == boost::uuids::uuid{{0}})
            continue;

        if (!saved_designs.count(uuid)) {
            WarnLogger() << "UUID " << uuid << " is in ship design manifest for "
                         << "a ship design that does not exist.";
            ship_manifest_inconsistent = true;
            continue;
        }
        ordering.push_back(uuid);
    }

    // Verify that every design in saved_designs is in ordering.
    if (ordering.size() != saved_designs.size()) {
        // Add any missing designs in alphabetical order to the end of the list
        std::unordered_set<boost::uuids::uuid, boost::hash<boost::uuids::uuid>>
            uuids_in_ordering{ordering.begin(), ordering.end()};
        std::map<std::string, boost::uuids::uuid> missing_uuids_sorted_by_name;
        for (auto& uuid_to_design_and_filename: saved_designs) {
            if (uuids_in_ordering.count(uuid_to_design_and_filename.first))
                continue;
            ship_manifest_inconsistent = true;
            missing_uuids_sorted_by_name.insert(
                std::make_pair(uuid_to_design_and_filename.second.first->Name(),
                               uuid_to_design_and_filename.first));
        }

        for (auto& name_and_uuid: missing_uuids_sorted_by_name) {
            WarnLogger() << "Missing ship design " << name_and_uuid.second
                         << " called " << name_and_uuid.first
                         << " added to the manifest.";
            ordering.push_back(name_and_uuid.second);
        }
    }

    return std::make_tuple(ship_manifest_inconsistent, std::move(saved_designs), ordering);
}
