#pragma once
#include "PCH.h"
#include "declarations/ConditionFlowXml.h"
#include "runtime/ConditionValues.h"

namespace AnimationInjector::ExtendedConditions {
ConditionFlow::Value Evaluate(const ConditionFlowXml::Predicate& predicate, RE::Actor* subject, RE::Actor* counterpart,
    int flow, int index, ConditionValues::Request& request);
}
