#include "declarations/ConditionFlowXml.h"
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string>
#include <utility>
using namespace AnimationInjector;
void Check(bool ok) {
    if (!ok) std::abort();
}
// One <Check> read alone (a card reads each the same way).
bool Read(const std::string& xml, ConditionFlowXml::Predicate& out) {
    pugi::xml_document doc;
    Check(doc.load_string(xml.c_str()));
    std::string error;
    const bool ok = ConditionFlowXml::ReadCheck(doc.document_element(), out, error);
    Check(ok == error.empty());
    return ok;
}
int main() {
    ConditionFlowXml::Predicate p;
    for (const char* subject : {"self", "partner"}) {
        Check(Read(std::string("<Check kind='female' subject='") + subject + "'/>", p));
        Check(p.test == ConditionFlowXml::Test::kFemale);
        Check(p.subject == (std::string_view(subject) == "self" ? ConditionFlowXml::Subject::kSelf
                                                                : ConditionFlowXml::Subject::kPartner));
    }
    Check(Read("<Check kind='stamina' minimum='10'/>", p) && p.minimum == 10);
    Check(Read("<Check kind='combat' subject='partner'/>", p) && p.subject == ConditionFlowXml::Subject::kPartner);
    // Room and ground (0.12.299): measured where the actor stands, from its own heading; no candidate, no partner.
    for (const auto& [name, angle] : std::initializer_list<std::pair<const char*, float>>{{"selfForward", 0.0f},
             {"selfBack", 3.141592654f}, {"selfRight", 1.570796327f}, {"selfLeft", -1.570796327f}}) {
        Check(Read(std::string("<Check kind='space' distance='250' direction='") + name + "'/>", p));
        Check(p.distance == 250 && std::abs(p.direction - angle) < 1e-6f);
    }
    Check(Read("<Check kind='ground' direction='selfForward' distance='0' maxRise='15' maxDrop='30' maxSlope='35' "
               "radius='18'/>",
        p));
    Check(p.ground.rise == 15 && p.ground.slope == 35 && p.distance == 0);
    Check(Read("<Check kind='magicEffect' subject='partner' plugin='FH_Grapple.esp' form='82C'/>", p));
    Check(p.test == ConditionFlowXml::Test::kMagicEffect && p.form == 0x82C &&
          p.subject == ConditionFlowXml::Subject::kPartner);
    Check(Read("<Check kind='global' plugin='FH_Grapple.esp' form='803' equals='1'/>", p));
    Check(p.test == ConditionFlowXml::Test::kGlobal && p.equals == 1);
    for (const char* bad : {"<Check kind='magicEffect' plugin='FH_Grapple.esp' form='FF00082C'/>",
             "<Check kind='magicEffect' plugin='FH_Grapple.esp' form='0'/>",
             "<Check kind='magicEffect' plugin='FH_Grapple.esp' form='82C' equals='1'/>",
             "<Check kind='magicEffect' form='82C'/>",
             "<Check kind='global' plugin='FH_Grapple.esp' form='803' equals='NaN'/>",
             "<Check kind='global' subject='partner' plugin='FH_Grapple.esp' form='803' equals='1'/>",
             "<Check kind='alive' plugin='FH_Grapple.esp' form='803'/>", "<Check kind='space'/>",
             "<Check kind='ground' direction='selfForward' distance='0' maxSlope='NaN'/>",
             "<Check kind='ground' direction='selfForward' distance='0' maxDrop='-1'/>",
             "<Check kind='ground' direction='selfForward' distance='0' radius='0'/>",
             "<Check kind='alive' maxRise='10'/>", "<Check kind='space' direction='forward' distance='200'/>",
             "<Check kind='space' direction='selfForward' distance='NaN'/>",
             "<Check kind='space' direction='selfForward' distance='0'/>",
             "<Check kind='space' direction='selfForward' distance='2001'/>",
             "<Check kind='space' direction='partnerBack' distance='250'/>",
             "<Check kind='space' subject='partner' direction='selfForward' distance='250'/>",
             "<Check kind='space' candidate='A' direction='selfForward' distance='250'/>",
             "<Check kind='alive' direction='selfForward'/>", "<Check kind='stamina' minimum='NaN'/>",
             "<Check kind='alive' subject='other'/>", "<Check kind='alive' minimum='1'/>",
             "<Check kind='unarmedRight'/>", "<Check kind='alive' pass='A'/>"}) {
        Check(!Read(bad, p));
    }
    std::cout << "Check XML reading and rejection passed\n";
}
