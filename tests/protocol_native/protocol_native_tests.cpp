#include "protocol_native/v1_codec.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc,char** argv){
 try{
  if(argc!=2) throw std::runtime_error{"root required"};
  std::ifstream in{std::string{argv[1]}+"/protocol/v1/fixtures/valid/response.ndjson"};
  std::vector<synth::protocol_native::Line> lines; std::string raw;
  while(std::getline(in,raw)) if(!raw.empty()) lines.push_back(synth::protocol_native::decode_line(raw));
  if(lines.size()!=4 || !std::holds_alternative<synth::protocol_native::Start>(lines[0].payload) || !std::holds_alternative<synth::protocol_native::Dialogue>(lines[1].payload) || !std::holds_alternative<synth::protocol_native::Action>(lines[2].payload) || !std::holds_alternative<synth::protocol_native::End>(lines[3].payload)) throw std::runtime_error{"fixture types"};
  const auto& dialogue=std::get<synth::protocol_native::Dialogue>(lines[1].payload);
  if(!dialogue.speech || dialogue.speech->cache_key!="0123456789abcdef0123456789abcdef" || dialogue.speech->status!="pending") throw std::runtime_error{"speech fixture"};
  {
    std::ifstream source{std::string{argv[1]}+"/protocol/v1/fixtures/valid/response.ndjson"};
    std::string legacy; std::getline(source,legacy); std::getline(source,legacy);
    legacy.insert(legacy.find("\"speaker\":"),"\"listener\":{\"form_id\":\"0x00000014\",\"origin_plugin\":\"Fallout4.esm\",\"playthrough_id\":\"save-1\"},");
    bool refused=false;
    try { (void)synth::protocol_native::decode_line(legacy); }
    catch(const std::invalid_argument&) { refused=true; }
    if(!refused) throw std::runtime_error{"v1 must reject listener extension"};
  }
  const auto& action=std::get<synth::protocol_native::Action>(lines[2].payload);
  if(action.idempotency_key!="idem-action-1" || lines[2].turn_id!="turn-text-1") throw std::runtime_error{"correlation"};
  std::ifstream sheathe_in{std::string{argv[1]}+"/protocol/v1/fixtures/valid/response-sheathe.ndjson"};
  std::getline(sheathe_in, raw);
  std::getline(sheathe_in, raw);
  const auto sheathe_line=synth::protocol_native::decode_line(raw);
  const auto& sheathe_action=std::get<synth::protocol_native::Action>(sheathe_line.payload);
  if(sheathe_action.name!="sheathe_weapon" || sheathe_action.capability!="action.sheathe_weapon" ||
     sheathe_action.actor.origin_plugin!="Fallout4.esm") throw std::runtime_error{"sheathe action fixture"};
  std::ifstream end_conversation_in{std::string{argv[1]}+"/protocol/v1/fixtures/valid/response-end-conversation.ndjson"};
  std::getline(end_conversation_in, raw);
  std::getline(end_conversation_in, raw);
  const auto end_conversation_line=synth::protocol_native::decode_line(raw);
  const auto& end_conversation_action=std::get<synth::protocol_native::Action>(end_conversation_line.payload);
  if(end_conversation_action.name!="end_conversation" ||
     end_conversation_action.capability!="action.end_conversation" ||
     end_conversation_line.runtime_variant!="vr") throw std::runtime_error{"end conversation action fixture"};
  synth::protocol_native::ActionResultEvent event{"session:flat:test","req-action-result-1","turn-text-1","flat","0.1.0","1.10.163",2,{"dialogue.text","action.inspect_actor"},action.action_id,action.idempotency_key,"succeeded","Actor is present in the current snapshot."};
  const auto encoded=synth::protocol_native::encode_action_result(event);
  const auto parsed=synth::json::parse(encoded);
  if(synth::protocol_native::text(parsed,"schema")!="synth.event.v1" || synth::protocol_native::text(synth::protocol_native::required(synth::protocol_native::required(parsed,"payload"),"result"),"idempotency_key")!="idem-action-1") throw std::runtime_error{"result encoding"};
  const synth::protocol_native::EventContext context{"session:flat:test","activate:2:1","turn:2:2","flat","0.1.0","1.11.240",2,{"agents.manage"}};
  const synth::protocol_native::Identity actor{"0x0001A4D7","Fallout4.esm","save:fo4:001","Preston Garvey"};
  {
    auto observed=event; observed.protocol_version=2; observed.context_sequence=7;
    observed.capabilities={"dialogue.turn_ownership","action.inventory_observation","context.inventory_512"};
    const synth::protocol_native::InventoryItem rifle{"0x00000001","Fallout4.esm","Rifle",1,43,100,8,true};
    observed.inventory=synth::protocol_native::ActionInventoryObservation{actor,{rifle},"complete"};
    for (const std::string quality:{"complete","partial","unavailable"}) {
      auto value=observed; value.inventory->observation=quality;
      if (quality=="unavailable") value.inventory->items.clear();
      const auto wire=synth::json::parse(synth::protocol_native::encode_action_result(value));
      const auto& result=synth::protocol_native::required(synth::protocol_native::required(wire,"payload"),"result");
      if (synth::protocol_native::text(result,"schema")!="synth.action.result.v2" ||
          synth::protocol_native::text(synth::protocol_native::required(result,"inventory"),"observation")!=quality)
        throw std::runtime_error{"post-action observation lost"};
    }
    auto extended=observed; extended.inventory->items.assign(512,rifle);
    (void)synth::protocol_native::encode_action_result(extended);
    for (int bad=0;bad<16;++bad) {
      auto value=observed;
      if(bad==0) value.capabilities.clear();
      if(bad==1) value.protocol_version=1;
      if(bad==2) value.context_sequence=0;
      if(bad==3) value.inventory->items.assign(513,rifle);
      if(bad==4) { value.inventory->items.assign(33,rifle); value.runtime_variant="vr"; }
      if(bad==5) { value.inventory->items.assign(33,rifle); value.capabilities.pop_back(); }
      if(bad==6) value.inventory->observation="unknown";
      if(bad==7) value.inventory->observation="unavailable";
      if(bad==8) value.inventory->actor.form_id="0xGG000001";
      if(bad==9) value.inventory->actor.playthrough_id="bad/save";
      if(bad==10) value.inventory->items[0].origin_plugin="bad.dll";
      if(bad==11) value.inventory->items[0].count=0;
      if(bad==12) value.inventory->items[0].weight=100001;
      if(bad==13) value.inventory->items[0].form_type=65536;
      if(bad==14) value.inventory->items[0].display_name="   ";
      if(bad==15) value.inventory->items[0].value=-1;
      bool refused=false;
      try { (void)synth::protocol_native::encode_action_result(value); }
      catch(const std::invalid_argument&) { refused=true; }
      if(!refused) throw std::runtime_error{"invalid inventory receipt encoded: "+std::to_string(bad)};
    }
  }
  {
    auto paired=event; paired.protocol_version=2; paired.context_sequence=7;
    paired.capabilities={"dialogue.turn_ownership","action.inventory_observation","action.give_item_to","action.transfer_inventory","context.inventory_512"};
    const synth::protocol_native::InventoryItem item{"0x00000001","Fallout4.esm","Rifle",1,43,100,8,false};
    paired.transfer=synth::protocol_native::ActionTransferObservation{{actor,{},"complete"},
      {{"0x00000014","Fallout4.esm",actor.playthrough_id,"Sole Survivor"},{item},"complete"}};
    for (const std::string quality:{"complete","partial","unavailable"}) {
      auto value=paired;value.transfer->recipient.observation=quality;
      if(quality=="unavailable") value.transfer->recipient.items.clear();
      const auto wire=synth::json::parse(synth::protocol_native::encode_action_result(value));
      const auto& result=synth::protocol_native::required(synth::protocol_native::required(wire,"payload"),"result");
      if(synth::protocol_native::text(result,"schema")!="synth.action.transfer-result.v2" ||
         synth::protocol_native::text(synth::protocol_native::required(result,"recipient_inventory"),"observation")!=quality ||
         synth::protocol_native::text(synth::protocol_native::required(result,"inventory"),"observation")!="complete")
        throw std::runtime_error{"paired observation qualities conflated"};
    }
    auto extended=paired;extended.transfer->donor.items.assign(512,item);extended.transfer->recipient.items.assign(512,item);
    (void)synth::protocol_native::encode_action_result(extended);
    for(int bad=0;bad<20;++bad) {
      auto value=paired;
      if(bad<4) value.capabilities.erase(value.capabilities.begin()+bad);
      if(bad==4) value.runtime_variant="vr";
      if(bad==5) value.protocol_version=1;
      if(bad==6) value.context_sequence=0;
      if(bad==7) value.inventory=value.transfer->donor;
      if(bad==8) value.transfer->recipient.actor=value.transfer->donor.actor;
      if(bad==9) {value.transfer->recipient.actor.form_id="0x0001a4d7";value.transfer->recipient.actor.origin_plugin="Other.esp";}
      if(bad==10) value.transfer->recipient.actor.playthrough_id="other:save";
      if(bad==11) value.transfer->recipient.items.assign(513,item);
      if(bad==12) {value.transfer->recipient.items.assign(33,item);value.capabilities.pop_back();}
      if(bad==13) {value.transfer->donor.items.assign(33,item);value.capabilities.pop_back();}
      if(bad==14) value.transfer->recipient.observation="unavailable";
      if(bad==15) value.transfer->recipient.actor.form_id="0xGG000014";
      if(bad==16) value.transfer->recipient.items[0].count=0;
      if(bad==17) value.transfer->recipient.items[0].origin_plugin="../Other.esp";
      if(bad==18) value.transfer->donor.observation="unknown";
      if(bad==19) {
        auto large=item;large.display_name=std::string(255,'\x01');large.origin_plugin=std::string(251,'\x01')+".esm";
        value.transfer->donor.items.assign(512,large);value.transfer->recipient.items.assign(512,large);
      }
      bool refused=false;
      try{(void)synth::protocol_native::encode_action_result(value);}
      catch(const std::invalid_argument&){refused=bad!=19;}
      catch(const synth::json::ByteLimitError&){refused=bad==19;}
      if(!refused) throw std::runtime_error{"invalid paired receipt encoded: "+std::to_string(bad)};
    }
  }
  const synth::protocol_native::LoadedPlugin plugin{"Fallout4.esm","00",false,0,0,0};
  const auto initialized=synth::json::parse(synth::protocol_native::encode_init(context,"en-US",false,{plugin}));
  if(!synth::protocol_native::required(synth::protocol_native::required(initialized,"payload"),"plugins").is_array()) throw std::runtime_error{"loaded plugin encoding"};
  const auto activation=synth::json::parse(synth::protocol_native::encode_activate(context,actor,"manual"));
  if(synth::protocol_native::text(activation,"type")!="activate" || synth::protocol_native::text(synth::protocol_native::required(activation,"payload"),"source")!="manual") throw std::runtime_error{"activation encoding"};
  const auto trigger=synth::json::parse(synth::protocol_native::encode_trigger(context,"rechat",actor));
  if(synth::protocol_native::text(trigger,"type")!="trigger" || synth::protocol_native::text(synth::protocol_native::required(trigger,"payload"),"kind")!="rechat") throw std::runtime_error{"trigger encoding"};
  auto owned_context=context; owned_context.protocol_version=2; owned_context.context_sequence=7;
  owned_context.capabilities={"dialogue.turn_ownership","dialogue.rechat.ownership"};
  const synth::protocol_native::RechatParent parent{"parent:request","parent:line"};
  const auto encode_rechat=[&](const auto& owner, std::string kind, std::optional<synth::protocol_native::RechatParent> origin) {
    return synth::protocol_native::encode_trigger(owner,std::move(kind),actor,std::nullopt,std::nullopt,std::nullopt,
      std::nullopt,std::nullopt,std::nullopt,std::move(origin));
  };
  for(const std::string lane:{"flat","vr"}) {
    owned_context.runtime_variant=lane;
    const auto wire=synth::json::parse(encode_rechat(owned_context,"rechat",parent));
    const auto& payload=synth::protocol_native::required(wire,"payload");
    if(synth::protocol_native::text(payload,"reply_to_request_id")!=parent.request_id ||
       synth::protocol_native::text(payload,"reply_to_line_id")!=parent.line_id)
      throw std::runtime_error{"exact rechat parent lost"};
  }
  for(int bad=0;bad<8;++bad) {
    auto owner=owned_context; auto origin=std::optional{parent}; std::string kind="rechat";
    if(bad==0) origin.reset();
    if(bad==1) origin->line_id="";
    if(bad==2) origin->request_id=std::string(129,'a');
    if(bad==3) origin->line_id="bad/line";
    if(bad==4) owner.capabilities.clear();
    if(bad==5) owner.protocol_version=1;
    if(bad==6) owner.context_sequence=0;
    if(bad==7) kind="bored";
    bool rejected=false;
    try { (void)encode_rechat(owner,kind,origin); } catch(const std::invalid_argument&) { rejected=true; }
    if(!rejected) throw std::runtime_error{"invalid rechat parent accepted"};
  }
    {
      synth::core::SceneIdentity physical{0x18AA2, 0x3C, "Fallout4.esm", "Fallout4.esm"};
      synth::protocol_native::WorldState world{"Place", "Duplicate Cell Name", "Commonwealth", "", false, 42, physical};
      const auto legacy=synth::json::write(synth::protocol_native::world_state_value(world));
      if(legacy.find("\"scene\"")!=std::string::npos) throw std::runtime_error{"scene leaked to unnegotiated peer"};
      const auto value=synth::protocol_native::world_state_value(world,true);
      const auto& scene=synth::protocol_native::required(value,"scene");
      if(synth::protocol_native::text(synth::protocol_native::required(scene,"cell"),"form_id")!="0x00018AA2")
        throw std::runtime_error{"physical cell identity lost"};
      auto replacement=physical; replacement.cell_form_id++;
      if(replacement==physical) throw std::runtime_error{"distinct cells collapsed"};
      replacement=physical; replacement.worldspace_origin_plugin="Other.esp";
      if(replacement==physical) throw std::runtime_error{"distinct worlds collapsed"};
      world.interior=true;
      bool rejected=false;
      try { (void)synth::protocol_native::world_state_value(world,true); } catch(const std::invalid_argument&) { rejected=true; }
      if(!rejected) throw std::runtime_error{"interior retained an exterior worldspace"};
      world.scene=synth::core::SceneIdentity{0x18AA2,0,"Fallout4.esm",""};
      (void)synth::protocol_native::world_state_value(world,true);
    }
    {
      auto owner=owned_context; owner.capabilities.push_back("dialogue.rechat.scene");
      auto fresh=parent; fresh.scene_context_sequence=8;
      const auto wire=synth::json::parse(encode_rechat(owner,"rechat",fresh));
      if(synth::protocol_native::integer(wire,"context_sequence")!=7 ||
         synth::protocol_native::integer(synth::protocol_native::required(wire,"payload"),"scene_context_sequence")!=8)
        throw std::runtime_error{"fresh scene replaced immutable chain ownership"};
      for(int bad=0;bad<6;++bad) {
        auto invalid=owner; auto scene=fresh;
        if(bad==0) scene.scene_context_sequence.reset();
        if(bad==1) invalid.capabilities.pop_back();
        if(bad==2) scene.scene_context_sequence=0;
        if(bad==3) scene.scene_context_sequence=7;
        if(bad==4) scene.scene_context_sequence=9007199254740992ULL;
        if(bad==5) invalid.protocol_version=1;
        bool rejected=false;
        try { (void)encode_rechat(invalid,"rechat",scene); } catch(const std::invalid_argument&) { rejected=true; }
        if(!rejected) throw std::runtime_error{"invalid fresh scene reference accepted"};
      }
    }
    const auto combat=synth::json::parse(synth::protocol_native::encode_trigger(context,"combat_bark",actor));
  if(synth::protocol_native::text(synth::protocol_native::required(combat,"payload"),"kind")!="combat_bark") throw std::runtime_error{"combat trigger encoding"};
  const auto greeting=synth::json::parse(synth::protocol_native::encode_trigger(context,"auto_greeting",actor,834000000000ULL));
  if(synth::protocol_native::integer(synth::protocol_native::required(greeting,"payload"),"game_time_ticks")!=834000000000ULL) throw std::runtime_error{"greeting time encoding"};
  const auto reaction=synth::json::parse(synth::protocol_native::encode_trigger(context,"external_reaction",actor,std::nullopt,std::string{"React to the explosion."}));
  if(synth::protocol_native::text(synth::protocol_native::required(reaction,"payload"),"instruction")!="React to the explosion.") throw std::runtime_error{"external reaction encoding"};
  const auto exact_tts=synth::json::parse(synth::protocol_native::encode_trigger(context,"external_tts",actor,std::nullopt,std::nullopt,std::string{"Keep this spacing."}));
  if(synth::protocol_native::text(synth::protocol_native::required(exact_tts,"payload"),"text")!="Keep this spacing.") throw std::runtime_error{"external TTS encoding"};
  const auto profile_refresh=synth::json::parse(synth::protocol_native::encode_profile_refresh(context,{actor},true));
  if(synth::protocol_native::text(profile_refresh,"type")!="profile_refresh" ||
     synth::protocol_native::required(synth::protocol_native::required(profile_refresh,"payload"),"actors").as_array().size()!=1 ||
     !synth::protocol_native::required(synth::protocol_native::required(profile_refresh,"payload"),"include_narrator").as_bool()) throw std::runtime_error{"profile refresh encoding"};
  const auto visual=synth::protocol_native::decode_visual_response("{\"schema\":\"synth.visual_context.response.v1\",\"request_id\":\"visual:2:1\",\"turn_id\":\"turn:2:2\",\"generation\":2,\"capture_id\":\"capture:2:3\",\"ok\":true,\"status\":\"success\",\"description\":\"A settlement.\"}");
  if(!visual.ok || visual.generation!=2 || visual.detail!="A settlement.") throw std::runtime_error{"visual response decoding"};
  const auto replayed_visual=synth::protocol_native::decode_visual_response("{\"schema\":\"synth.visual_context.response.v1\",\"request_id\":\"visual:2:1\",\"turn_id\":\"turn:2:2\",\"generation\":2,\"capture_id\":\"capture:2:3\",\"ok\":true,\"status\":\"success\",\"record_id\":1,\"replayed\":true}");
  if(!replayed_visual.ok) throw std::runtime_error{"visual replay decoding"};
  const synth::protocol_native::ActorState state{actor,"0x0001A4D7","Fallout4.esm",10.0,20.0,30.0,125.0,true,false,false,false,false,10,
                                                  "HumanRace","male","MaleBoston",true,82.5,65.0,
                                                  {{"0x0001F66A","Fallout4.esm","Laser Musket",1,41,57,8.0,true}},
                                                  "visible",
                                                  {{"0x0005DE41","Fallout4.esm","MinutemenFaction","MinutemenFaction",0}},
                                                  "effective","alive","normal",false,12.5,false,true,false,
                                                  {true,"0x000A2420","Fallout4.esm","SandboxPackage","SandboxPackage"},"complete"};
  const synth::protocol_native::WorldState world{"Sanctuary Hills","SanctuaryExt","Commonwealth","CommonwealthClear",false,834000000000ULL};
  const synth::protocol_native::QuestState quest{"0x000229E5","Fallout4.esm","When Freedom Calls","Min00",45,1,{"Join Preston Garvey in Sanctuary"}};
  const synth::protocol_native::NearbyItemState nearby_item{"0xFF001234","0x00004822","0x00018AA2",std::nullopt,"Fallout4.esm","Fallout4.esm","10mm Pistol",12,22,30,14.14,4.2,1,43,50,false,true,false};
  const synth::protocol_native::PointOfInterestState poi{"0x0001A6D8","0x0001A6D7","0x00018AA2",std::string{"Fallout4.esm"},"Fallout4.esm","Fallout4.esm","Sanctuary workshop door","door",40,20,30,40,false,false};
  const auto context_event=synth::json::parse(synth::protocol_native::encode_context(context,"save:fo4:001",12,actor,actor,{actor},{state},world,{quest},{nearby_item},{poi},std::nullopt,"complete","partial","cached","partial"));
  const auto& environment_payload=synth::protocol_native::required(context_event,"payload");
  if(synth::protocol_native::text(environment_payload,"audience_observation")!="partial") throw std::runtime_error{"audience quality encoding"};
  for(const auto quality : {"invented","cached","unavailable"}) {
    bool rejected=false;
    try { (void)synth::protocol_native::encode_context(context,"save:fo4:001",12,actor,actor,{actor},{},{},{},{},{},{},{},{},{},quality); }
    catch(const std::invalid_argument&) {rejected=true;}
    if(!rejected) throw std::runtime_error{"invalid audience quality accepted"};
  }
  if(synth::protocol_native::text(environment_payload,"nearby_items_observation")!="partial" || synth::protocol_native::text(environment_payload,"points_of_interest_observation")!="cached") throw std::runtime_error{"environment quality encoding"};
  for (const auto quality : {"complete","partial","cached","unavailable"}) {
      if(synth::protocol_native::retained_collection_observation(quality,0,0)!=quality ||
         synth::protocol_native::retained_collection_observation(quality,2,1)!=(std::string_view{quality}=="complete"?"partial":quality)) throw std::runtime_error{"environment cap quality"};
  }
  if(synth::protocol_native::text(synth::protocol_native::required(context_event,"payload"),"active_quests_observation")!="complete") throw std::runtime_error{"quest collection quality encoding"};
  if(!synth::protocol_native::required(synth::protocol_native::required(context_event,"payload"),"actor_states").is_array()) throw std::runtime_error{"actor state encoding"};
  const auto& encoded_state = synth::protocol_native::required(synth::protocol_native::required(context_event,"payload"),"actor_states").as_array().front();
  if(synth::protocol_native::required(encoded_state,"base_form_id").as_string() != "0x0001A4D7" ||
     synth::protocol_native::required(encoded_state,"base_origin_plugin").as_string() != "Fallout4.esm" ||
     synth::protocol_native::required(encoded_state,"voice_type").as_string() != "MaleBoston" ||
     !synth::protocol_native::required(encoded_state,"teammate").as_bool()) throw std::runtime_error{"actor semantics encoding"};
  if(synth::protocol_native::required(encoded_state,"inventory").as_array().size()!=1) throw std::runtime_error{"inventory encoding"};
  if(synth::protocol_native::text(encoded_state,"inventory_observation")!="complete") throw std::runtime_error{"inventory completeness encoding"};
  for (int mode=0;mode<5;++mode) {
      auto owner=context;owner.protocol_version=mode==0?1:2;owner.runtime_variant=mode==2?"vr":"flat";
      if(mode!=1) owner.capabilities.push_back("context.inventory_512");
      auto inventory_state=state;inventory_state.inventory.assign(mode==4?513:512,state.inventory.front());
      bool rejected=false;
      try { (void)synth::protocol_native::encode_context(owner,"save:fo4:001",12,actor,actor,{actor},{inventory_state}); }
      catch(const std::invalid_argument&) {rejected=true;}
      if(rejected!=(mode!=3)) throw std::runtime_error{"extended inventory codec capability boundary"};
  }
  for (const bool health_available : {false,true}) {
      for (const bool ap_available : {false,true}) {
          auto measured=state;measured.health_percent_available=health_available;measured.action_points_percent_available=ap_available;
          measured.health_percent=0;
          const auto encoded_values=synth::protocol_native::actor_state_value(measured);
          if(synth::protocol_native::required(encoded_values,"health_percent_available").as_bool()!=health_available ||
             synth::protocol_native::required(encoded_values,"action_points_percent_available").as_bool()!=ap_available ||
             synth::protocol_native::required(encoded_values,"health_percent").as_double()!=0) throw std::runtime_error{"actor value availability encoding"};
      }
  }
  for (const auto observation : {"complete", "partial", "unavailable", "invalid"}) {
      const auto result = synth::protocol_native::inventory_action_result(actor, {}, observation);
      const bool available = std::string_view{observation} == "complete" || std::string_view{observation} == "partial";
      if (result.first != (available ? "succeeded" : "unavailable")) throw std::runtime_error{"inventory availability action"};
      if (available) {
          const auto inventory_detail = synth::json::parse(result.second);
          if (synth::protocol_native::text(inventory_detail,"inventory_observation") != observation ||
              synth::protocol_native::required(inventory_detail,"truncated").as_bool() != (std::string_view{observation} == "partial")) throw std::runtime_error{"empty inventory quality"};
      }
  }
  const auto large_item = synth::json::Value{synth::protocol_native::object({{"display_name", std::string(2000,'x')}})};
  const auto capped_inventory = synth::protocol_native::inventory_action_result(actor, {large_item}, "complete");
  const auto capped_detail = synth::json::parse(capped_inventory.second);
  if (capped_inventory.second.size()>1024 || capped_inventory.first!="succeeded" ||
      !synth::protocol_native::required(capped_detail,"items").as_array().empty() ||
      synth::protocol_native::text(capped_detail,"inventory_observation")!="partial" ||
      !synth::protocol_native::required(capped_detail,"truncated").as_bool()) throw std::runtime_error{"inventory byte limit loses truncation"};
  if(synth::protocol_native::required(encoded_state,"factions").as_array().size()!=1 ||
     synth::protocol_native::text(encoded_state,"faction_observation")!="effective" ||
     synth::protocol_native::text(encoded_state,"faction_completeness")!="partial") throw std::runtime_error{"faction encoding"};
  for (const auto quality : {"complete","partial","unavailable"}) {
      auto explicit_state=state;
      explicit_state.faction_completeness=quality;
      if (std::string_view{quality}=="unavailable") {explicit_state.faction_observation="unavailable";explicit_state.factions.clear();}
      const auto encoded_factions=synth::protocol_native::actor_state_value(explicit_state);
      if(synth::protocol_native::text(encoded_factions,"faction_completeness")!=quality) throw std::runtime_error{"explicit faction completeness encoding"};
  }
  if(synth::protocol_native::text(encoded_state,"life_state")!="alive" ||
     !synth::protocol_native::required(encoded_state,"talking_to_player").as_bool() ||
     synth::protocol_native::text(synth::protocol_native::required(encoded_state,"current_package"),"editor_id")!="SandboxPackage") throw std::runtime_error{"activity encoding"};
  if(synth::protocol_native::text(synth::protocol_native::required(synth::protocol_native::required(context_event,"payload"),"world"),"location")!="Sanctuary Hills") throw std::runtime_error{"world state encoding"};
  if(synth::protocol_native::text(synth::protocol_native::required(synth::protocol_native::required(context_event,"payload"),"active_quests").as_array().front(),"display_name")!="When Freedom Calls") throw std::runtime_error{"quest state encoding"};
  if(synth::protocol_native::required(synth::protocol_native::required(synth::protocol_native::required(context_event,"payload"),"active_quests").as_array().front(),"objectives").as_array().size()!=1) throw std::runtime_error{"quest objectives encoding"};
  for (const auto quality : {"complete","partial","unavailable","cached"}) {
      const auto result=synth::protocol_native::quest_action_result({},quality);
      const bool fresh=std::string_view{quality}=="complete" || std::string_view{quality}=="partial";
      if(result.first!=(fresh?"succeeded":"unavailable")) throw std::runtime_error{"quest freshness action"};
      if(fresh && synth::protocol_native::text(synth::json::parse(result.second),"active_quests_observation")!=quality) throw std::runtime_error{"quest empty quality"};
  }
  const auto capped_quests=synth::protocol_native::quest_action_result({large_item},"complete");
  synth::protocol_native::QuestState tracked_quest{"0x000229E5","Fallout4.esm","Quest","QuestEditor",45,1,{"Find settler"},"complete"};
  if(synth::protocol_native::quest_state_value(tracked_quest,true).find("tracked")) throw std::runtime_error{"unobserved tracking encoded"};
  for (const auto tracked : {false,true}) {
      tracked_quest.tracked=tracked;
      if(synth::protocol_native::quest_state_value(tracked_quest).find("tracked")) throw std::runtime_error{"v1 quest tracking leaked"};
      if(synth::protocol_native::required(synth::protocol_native::quest_state_value(tracked_quest,true),"tracked").as_bool()!=tracked)
          throw std::runtime_error{"tracking boolean lost"};
  }
  if(capped_quests.second.size()>1024 || synth::protocol_native::text(synth::json::parse(capped_quests.second),"active_quests_observation")!="partial") throw std::runtime_error{"quest byte limit quality"};
  if(synth::protocol_native::text(synth::protocol_native::required(synth::protocol_native::required(context_event,"payload"),"nearby_items").as_array().front(),"display_name")!="10mm Pistol") throw std::runtime_error{"nearby item encoding"};
  if(synth::protocol_native::text(synth::protocol_native::required(synth::protocol_native::required(context_event,"payload"),"points_of_interest").as_array().front(),"kind")!="door") throw std::runtime_error{"point of interest encoding"};
  bool rejected=false; try{ (void)synth::protocol_native::decode_line("{\"schema\":\"synth.response.line.v1\"}"); }catch(...){rejected=true;} if(!rejected) throw std::runtime_error{"invalid accepted"};
  rejected=false; try{ (void)synth::protocol_native::decode_line("{\"schema\":\"synth.response.line.v1\",\"protocol_version\":1,\"type\":\"response_end\",\"line_id\":\"line:1\",\"request_id\":\"req:1\",\"turn_id\":\"turn:1\",\"generation\":1,\"game\":\"fo4\",\"runtime_variant\":\"flat\",\"payload\":{\"status\":\"complete\"},\"unexpected\":true}"); }catch(...){rejected=true;} if(!rejected) throw std::runtime_error{"unknown response field accepted"};
  const auto ready=synth::protocol_native::decode_tts_status("{\"schema\":\"synth.tts_status.v1\",\"cache_key\":\"0123456789abcdef0123456789abcdef\",\"status\":\"ready\",\"media\":{\"schema\":\"synth.media.v1\",\"media_id\":\"media/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"content_type\":\"audio/wav\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"size\":44}}");
  if(!ready.media || ready.status!="ready") throw std::runtime_error{"ready TTS status"};
  rejected=false; try{ (void)synth::protocol_native::decode_tts_status("{\"schema\":\"synth.tts_status.v1\",\"cache_key\":\"0123456789abcdef0123456789abcdef\",\"status\":\"pending\",\"path\":\"/tmp/voice.wav\"}"); }catch(...){rejected=true;} if(!rejected) throw std::runtime_error{"TTS path leak accepted"};
  rejected=false; try{ (void)synth::protocol_native::decode_visual_response("{\"schema\":\"synth.visual_context.response.v1\",\"request_id\":\"visual:2:1\",\"turn_id\":\"turn:2:2\",\"generation\":2,\"capture_id\":\"capture:2:3\",\"ok\":true,\"status\":\"success\",\"replayed\":\"yes\"}"); }catch(...){rejected=true;} if(!rejected) throw std::runtime_error{"invalid visual replay flag accepted"};
  std::ifstream v2_in{std::string{argv[1]}+"/protocol/v2/fixtures/valid/response.ndjson"};
  std::size_t v2_lines{};
  while(std::getline(v2_in,raw)) {
   const auto v2_line=synth::protocol_native::decode_line(raw);
   if(v2_line.protocol_version!=2 || v2_line.context_sequence!=7) throw std::runtime_error{"v2 response binding"};
   ++v2_lines;
  }
  if(v2_lines!=4) throw std::runtime_error{"v2 fixture count"};
  auto bound_context=context;
  bound_context.protocol_version=2;
  bound_context.context_sequence=7;
  const auto diary=synth::json::parse(synth::protocol_native::encode_diary_request(bound_context,actor));
  const auto& diary_payload=synth::protocol_native::required(diary,"payload");
  if(synth::protocol_native::text(diary,"type")!="diary_request" || synth::protocol_native::text(diary_payload,"reason")!="manual"
     || synth::protocol_native::required(diary_payload,"include_player").as_bool()
     || synth::protocol_native::required(diary_payload,"include_narrator").as_bool()
     || synth::protocol_native::required(diary_payload,"actors").as_array().size()!=1) throw std::runtime_error{"diary request encoding"};
  const auto v2_input=synth::json::parse(synth::protocol_native::encode_input_text(bound_context,"bound input"));
  const auto narrator_diary=synth::json::parse(synth::protocol_native::encode_diary_request(bound_context,actor,synth::protocol_native::DiaryRole::narrator));
  const auto& narrator_payload=synth::protocol_native::required(narrator_diary,"payload");
  if(!synth::protocol_native::required(narrator_payload,"include_narrator").as_bool()
     || !synth::protocol_native::required(narrator_payload,"actors").as_array().empty()
     || synth::protocol_native::required(narrator_payload,"include_player").as_bool()) throw std::runtime_error{"Narrator diary encoding"};
  if(synth::protocol_native::text(v2_input,"schema")!="synth.event.v2" ||
     synth::protocol_native::integer(v2_input,"context_sequence")!=7) throw std::runtime_error{"v2 input encoding"};
  event.protocol_version=2;
  event.context_sequence=7;
  const auto v2_result=synth::json::parse(synth::protocol_native::encode_action_result(event));
  if(synth::protocol_native::integer(v2_result,"protocol_version")!=2 ||
     synth::protocol_native::integer(v2_result,"context_sequence")!=7) throw std::runtime_error{"v2 result encoding"};
  bound_context.context_sequence=0;
  rejected=false;
  try { (void)synth::protocol_native::encode_input_text(bound_context,"unbound"); }
  catch(const std::invalid_argument&) { rejected=true; }
  if(!rejected) throw std::runtime_error{"v2 unbound input accepted"};
  const synth::core::SavedContext saved{"save:fo4:001", "session:previous", 2, 42};
  bound_context.capabilities.push_back("context.saved_anchor");
  const auto saved_event = synth::json::parse(synth::protocol_native::encode_context(
      bound_context,"save:fo4:001",13,actor,std::nullopt,{}, {},std::nullopt,{}, {}, {}, saved));
  const auto& saved_payload = synth::protocol_native::required(
      synth::protocol_native::required(saved_event,"payload"),"loaded_context");
  if(synth::protocol_native::integer(saved_payload,"context_sequence")!=42 ||
     synth::protocol_native::text(saved_payload,"session_id")!="session:previous")
      throw std::runtime_error{"saved context encoding"};
  for (int invalid = 0; invalid < 3; ++invalid) {
      auto test_context = bound_context;
      auto test_saved = saved;
      if (invalid == 0) test_context.protocol_version = 1;
      if (invalid == 1) test_context.capabilities.clear();
      if (invalid == 2) test_saved.playthrough_id = "other-player";
      rejected = false;
      try { (void)synth::protocol_native::encode_context(test_context,"save:fo4:001",13,actor,
          std::nullopt,{}, {},std::nullopt,{}, {}, {}, test_saved); }
      catch(const std::invalid_argument&) { rejected = true; }
      if(!rejected) throw std::runtime_error{"invalid saved context accepted"};
  }
  auto quest_context=context; quest_context.protocol_version=2; quest_context.capabilities.push_back("context.quest_events");
  const synth::protocol_native::NativeQuestBatch native_batch{"quest:1",{
      {{synth::core::QuestEventKind::stage,0x229E5,20,1,false},"Fallout4.esm",0},
      {{synth::core::QuestEventKind::stopped,0x229E5,0,0,true},"Fallout4.esm",5}}};
  const auto encode_quests=[&](const auto& owner,const auto& batch) {
      return synth::protocol_native::encode_context(owner,"save:fo4:001",13,actor,std::nullopt,{},
          {},std::nullopt,{}, {}, {}, std::nullopt, {}, {}, {}, {}, batch);
  };
  const auto quest_wire=synth::json::parse(encode_quests(quest_context,native_batch));
  const auto& quest_payload=synth::protocol_native::required(synth::protocol_native::required(quest_wire,"payload"),"quest_events");
  const auto& transitions=synth::protocol_native::required(quest_payload,"events").as_array();
  if(transitions.size()!=2 || synth::protocol_native::text(quest_payload,"batch_id")!="quest:1" ||
      synth::protocol_native::text(transitions[0],"form_id")!="0x000229E5" ||
      synth::protocol_native::integer(transitions[0],"stage")!=20 ||
      synth::protocol_native::integer(transitions[0],"item")!=1 ||
      synth::protocol_native::text(transitions[1],"kind")!="stopped" ||
      synth::protocol_native::integer(transitions[1],"visibility_sequence")!=5)
      throw std::runtime_error{"native quest scalars or ordering lost"};
  for(int invalid=0;invalid<13;++invalid) {
      auto owner=quest_context; auto batch=native_batch;
      if(invalid==0)owner.protocol_version=1;
      if(invalid==1)owner.runtime_variant="vr";
      if(invalid==2)owner.capabilities.clear();
      if(invalid==3)batch.batch_id="bad batch";
      if(invalid==4)batch.events.clear();
      if(invalid==5)batch.events.resize(33,native_batch.events[0]);
      if(invalid==6)batch.events[0].event.form_id=0;
      if(invalid==7)batch.events[0].event.failed=true;
      if(invalid==8)batch.events[0].visibility_sequence=13;
      if(invalid==9)batch.events[1].event.stage=1;
      if(invalid==10)batch.events[0].origin_plugin="../Fallout4.esm";
      if(invalid==11)batch.events[0].origin_plugin=".esm";
      if(invalid==12)batch.events[0].event.kind=static_cast<synth::core::QuestEventKind>(99);
      rejected=false;
      try{(void)encode_quests(owner,batch);}catch(const std::invalid_argument&){rejected=true;}
      if(!rejected)throw std::runtime_error{"invalid native quest batch accepted"};
  }
  auto maximum=native_batch;maximum.events.resize(32,native_batch.events[0]);
  (void)encode_quests(quest_context,maximum);
  const auto without=synth::json::parse(encode_quests(context,std::nullopt));
  if(synth::protocol_native::required(without,"payload").find("quest_events"))throw std::runtime_error{"legacy context gained native quests"};
  {
    auto control_context = context;
    using namespace synth::protocol_native;
    control_context.protocol_version = 2;
    control_context.context_sequence = 1;
    control_context.capabilities.push_back("control.menu");
    const auto turn = synth::json::parse(encode_input_text(control_context, "Hello", {"WHISPER", 2}));
    const auto& selection = required(required(turn, "payload"), "control");
    if (text(selection, "mode") != "WHISPER" || integer(selection, "model_slot") != 2)
      throw std::runtime_error{"control selection was not frozen into the turn"};
    bool bad{};
    try { (void)encode_input_text(control_context, "Hello", {"invalid", 5}); }
    catch (const std::invalid_argument&) { bad = true; }
    if (!bad) throw std::runtime_error{"invalid control selection accepted"};
    control_context.context_sequence = 0;
    const auto rpc = synth::json::parse(encode_control(control_context, "status", ""));
    if (integer(rpc, "context_sequence") != 0 || text(rpc, "type") != "control")
      throw std::runtime_error{"control RPC incorrectly owns a dialogue context"};
  }
  std::cout<<"protocol native tests passed\n"; return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
