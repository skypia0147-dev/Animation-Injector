#include "declarations/ConditionFlowXml.h"
#include <cstdlib>
#include <iostream>
using namespace AnimationInjector;
void Check(bool ok) {
    if (!ok) std::abort();
}
int main() {
    const char* text = R"(<Flow entry="HasResource">
      <Test id="HasResource" kind="stamina" minimum="10" pass="CheckAir" fail="Fallback"/>
      <Test id="CheckAir" kind="air" subject="self" pass="Air" fail="Fallback"/>
      <Play id="Air" action="Jump"/><Play id="Fallback" action="Idle"/>
    </Flow>)";
    pugi::xml_document doc;
    Check(doc.load_string(text));
    ConditionFlowXml::Program program;
    std::string error;
    for (const char* subject : {"self", "partner"}) {
        const auto xml =
            std::string("<Flow entry='Inverse'><Test id='Female' kind='female' subject='") + subject +
            "'/><Not id='Inverse' pass='Yes' fail='No'><Use node='Female'/></Not><Play id='Yes' action='Jump'/><Play id='No' action='Idle'/></Flow>";
        pugi::xml_document female;
        Check(female.load_string(xml.c_str()));
        Check(ConditionFlowXml::Read(female.document_element(), {"Jump", "Idle"}, program, error));
        Check(program.predicates[0].test == ConditionFlowXml::Test::kFemale);
        Check(program.predicates[0].subject == (std::string_view(subject) == "self"
                                                       ? ConditionFlowXml::Subject::kSelf
                                                       : ConditionFlowXml::Subject::kPartner));
        for (auto value :
            {ConditionFlow::Value::kTrue, ConditionFlow::Value::kFalse, ConditionFlow::Value::kUnavailable}) {
            const auto result = ConditionFlow::Select(program.nodes, program.entry, 1, 2, [&](int) { return value; });
            Check(result.action == (value == ConditionFlow::Value::kFalse ? 0 : 1));
            Check(result.unavailable == (value == ConditionFlow::Value::kUnavailable));
        }
    }
    Check(ConditionFlowXml::Read(doc.document_element(), {"Jump", "Idle"}, program, error));
    Check(program.predicates[0].minimum == 10);
    const auto result = ConditionFlow::Select(
        program.nodes, program.entry, program.predicates.size(), 2, [](int) { return ConditionFlow::Value::kTrue; });
    Check(result.action == 0);
    Check(doc.load_string(
        R"(<Flow entry="Choose"><Any id="Choose" pass="Play" fail="Stop"><Use node="Combat"/><Use node="NotAir"/></Any><Test id="Combat" kind="combat" subject="partner"/><Not id="NotAir"><Use node="Air"/></Not><Test id="Air" kind="air"/><Play id="Play" action="Idle"/><Stop id="Stop"/></Flow>)"));
    Check(ConditionFlowXml::Read(doc.document_element(), {"Jump", "Idle"}, program, error));
    Check(program.predicates[0].subject == ConditionFlowXml::Subject::kPartner);
    Check(ConditionFlow::Select(program.nodes, program.entry, program.predicates.size(), 2, [](int i) {
        return i ? ConditionFlow::Value::kFalse : ConditionFlow::Value::kTrue;
    }).action == 1);
    const auto missingPartner = ConditionFlow::Select(program.nodes, program.entry, program.predicates.size(), 2,
        [](int i) { return i ? ConditionFlow::Value::kFalse : ConditionFlow::Value::kUnavailable; });
    Check(missingPartner.action == ConditionFlow::absent && missingPartner.unavailable);
    Check(doc.load_string(
        R"(<Flow entry="Room"><Test id="Room" kind="space" candidate="Jump" subject="partner" direction="partnerBack" distance="250" pass="Move" fail="Still"/><Play id="Move" action="Jump"/><Play id="Still" action="Idle"/></Flow>)"));
    Check(ConditionFlowXml::Read(doc.document_element(), {"Jump", "Idle"}, program, error));
    Check(program.predicates[0].candidate == 0 && program.predicates[0].distance == 250 &&
          program.predicates[0].direction > 3.14f);
    Check(doc.load_string(
        R"(<Flow entry="Floor"><Test id="Floor" kind="ground" candidate="Idle" direction="partnerForward" distance="0" maxRise="15" maxDrop="30" maxSlope="35" radius="18" pass="Play"/><Play id="Play" action="Idle"/></Flow>)"));
    Check(ConditionFlowXml::Read(doc.document_element(), {"Jump", "Idle"}, program, error));
    Check(program.predicates[0].ground.rise == 15 && program.predicates[0].ground.slope == 35 &&
          program.predicates[0].distance == 0);
    Check(doc.load_string(
        R"(<Flow entry="Room"><Test id="Room" kind="space" candidate="Jump" subject="partner" direction="partnerBack" distance="250" pass="Move" fail="Still"/><Play id="Move" action="Jump"/><Play id="Still" action="Idle"/></Flow>)"));
    Check(ConditionFlowXml::Read(doc.document_element(), {"Jump", "Idle"}, program, error));
    for (auto value : {ConditionFlow::Value::kTrue, ConditionFlow::Value::kFalse, ConditionFlow::Value::kUnavailable}) {
        const auto selected = ConditionFlow::Select(
            program.nodes, program.entry, program.predicates.size(), 2, [&](int) { return value; });
        Check(selected.action == (value == ConditionFlow::Value::kTrue ? 0 : 1));
        Check(selected.unavailable == (value == ConditionFlow::Value::kUnavailable));
    }
    Check(doc.load_string(
        R"(<Flow entry="Effect"><Test id="Effect" kind="magicEffect" subject="partner" plugin="FH_Grapple.esp" form="82C" pass="Mode"/><Test id="Mode" kind="global" plugin="FH_Grapple.esp" form="803" equals="1"/></Flow>)"));
    Check(ConditionFlowXml::Read(doc.document_element(), {"Jump", "Idle"}, program, error));
    Check(program.predicates[0].test == ConditionFlowXml::Test::kMagicEffect && program.predicates[0].form == 0x82C &&
          program.predicates[0].subject == ConditionFlowXml::Subject::kPartner);
    Check(program.predicates[1].test == ConditionFlowXml::Test::kGlobal && program.predicates[1].equals == 1);
    for (const char* bad :
        {"<Flow entry='A'><Test id='A' kind='magicEffect' plugin='FH_Grapple.esp' form='FF00082C'/></Flow>",
            "<Flow entry='A'><Test id='A' kind='magicEffect' plugin='FH_Grapple.esp' form='0'/></Flow>",
            "<Flow entry='A'><Test id='A' kind='magicEffect' plugin='FH_Grapple.esp' form='82C' equals='1'/></Flow>",
            "<Flow entry='A'><Test id='A' kind='magicEffect' form='82C'/></Flow>",
            "<Flow entry='A'><Test id='A' kind='global' plugin='FH_Grapple.esp' form='803' equals='NaN'/></Flow>",
            "<Flow entry='A'><Test id='A' kind='global' subject='partner' plugin='FH_Grapple.esp' form='803' equals='1'/></Flow>",
            "<Flow entry='A'><Test id='A' kind='alive' plugin='FH_Grapple.esp' form='803'/></Flow>",
            "<Flow entry='A'><Test id='A' kind='space'/></Flow>",
            "<Flow entry='A'><Test id='A' kind='ground' candidate='Idle' direction='partnerForward' distance='0' maxSlope='NaN'/></Flow>",
            "<Flow entry='A'><Test id='A' kind='ground' candidate='Idle' direction='partnerForward' distance='0' maxDrop='-1'/></Flow>",
            "<Flow entry='A'><Test id='A' kind='ground' candidate='Idle' direction='partnerForward' distance='0' radius='0'/></Flow>",
            "<Flow entry='A'><Test id='A' kind='alive' maxRise='10'/></Flow>",
            "<Flow entry='A'><Test id='A' kind='space' candidate='Missing' direction='partnerForward' distance='200'/></Flow>",
            "<Flow entry='A'><Test id='A' kind='space' candidate='Idle' direction='forward' distance='200'/></Flow>",
            "<Flow entry='A'><Test id='A' kind='space' candidate='Idle' direction='partnerForward' distance='NaN'/></Flow>",
            "<Flow entry='A'><Test id='A' kind='space' candidate='Idle' direction='partnerForward' distance='0'/></Flow>",
            "<Flow entry='A'><Test id='A' kind='space' candidate='Idle' direction='partnerForward' distance='2001'/></Flow>",
            "<Flow entry='A'><Test id='A' kind='alive' candidate='Idle'/></Flow>",
            "<Flow entry='A'><Play id='A' action='Missing'/></Flow>",
            "<Flow entry='A'><Test id='A' kind='alive' pass='A'/></Flow>",
            "<Flow entry='A'><Test id='A' kind='stamina' minimum='NaN'/></Flow>",
            "<Flow entry='A'><Stop id='A'/><Stop id='A'/></Flow>", "<Flow entry='A'><Not id='A'/></Flow>",
            "<Flow entry='A'><Test id='A' kind='alive' subject='other'/></Flow>",
            "<Flow entry='A'><All id='A'><Use node='B'/></All><Not id='B'><Use node='A'/></Not></Flow>",
            "<Flow entry='A'><Stop id='A' pass='B'/><Stop id='B'/></Flow>",
            "<Flow entry='A'><Test id='A' kind='alive' minimum='1'/></Flow>",
            "<Flow entry='A'><Test id='A' kind='air' fail='Missing'/></Flow>"}) {
        Check(doc.load_string(bad));
        Check(!ConditionFlowXml::Read(doc.document_element(), {"Jump", "Idle"}, program, error));
        Check(program.nodes.empty() && !error.empty());
    }
    std::cout << "Flow XML resolution, selection and atomic rejection passed\n";
}
