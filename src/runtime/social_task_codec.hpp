#pragma once
#include "runtime/social_task.hpp"
#include <charconv>
#include <cstdio>

namespace synth::runtime {
inline void social_closed(const json::Value& value,std::initializer_list<std::string_view> allowed) {
    if(!value.is_object())throw std::invalid_argument{"social value is not object"};
    std::vector<std::string_view> seen;
    for(const auto& [key,ignored]:value.as_object()) {
        (void)ignored;
        if(std::ranges::find(allowed,key)==allowed.end()||std::ranges::find(seen,key)!=seen.end())throw std::invalid_argument{"unknown or duplicate social field"};
        seen.push_back(key);
    }
}
inline const json::Value& social_required(const json::Value& value,std::string_view key) {
    const auto* found=value.find(key);if(!found)throw std::invalid_argument{"missing social field"};
    return *found;
}
inline std::string social_string(const json::Value& value,std::string_view key,std::size_t limit,bool empty=false) {
    const auto& found=social_required(value,key);
    if(!found.is_string()||found.as_string().size()>limit||(!empty&&found.as_string().empty())||found.as_string().find('\0')!=std::string::npos||!json::detail::valid_utf8(found.as_string()))throw std::invalid_argument{"invalid social text"};
    return found.as_string();
}
inline std::uint64_t social_integer(const json::Value& value) {
    if(const auto* n=std::get_if<std::uint64_t>(&value.storage())) {if(*n<=9'007'199'254'740'991ULL)return *n;}
    if(const auto* n=std::get_if<std::int64_t>(&value.storage())) {if(*n>=0&&*n<=9'007'199'254'740'991LL)return static_cast<std::uint64_t>(*n);}
    throw std::invalid_argument{"invalid social integer"};
}
inline std::uint32_t social_form(const json::Value& value) {
    const auto text=social_string(value,"form_id",10);std::uint32_t id{};
    if(text.size()!=10||!text.starts_with("0x"))throw std::invalid_argument{"invalid social form"};
    const auto result=std::from_chars(text.data()+2,text.data()+10,id,16);
    if(result.ec!=std::errc{}||result.ptr!=text.data()+10||!id)throw std::invalid_argument{"invalid social form"};
    return id;
}
inline RuntimeActorIdentity social_decode_actor(const json::Value& value) {
    social_closed(value,{"form_id","origin_plugin","playthrough_id","display_name"});
    auto actor=RuntimeActorIdentity{social_form(value),social_string(value,"origin_plugin",255),social_string(value,"playthrough_id",128)};
    if(value.find("display_name"))(void)social_string(value,"display_name",255);
    if(!social_actor_valid(actor))throw std::invalid_argument{"invalid social actor"};
    return actor;
}
inline json::Value social_encode_actor(const RuntimeActorIdentity& actor,std::string_view name={}) {
    if(!social_actor_valid(actor))throw std::invalid_argument{"invalid social actor"};
    char form[11]{};std::snprintf(form,sizeof(form),"0x%08X",actor.form_id);
    json::Value::Object object{{"form_id",form},{"origin_plugin",actor.origin_plugin},{"playthrough_id",actor.playthrough_id}};
    if(!name.empty())object.emplace_back("display_name",std::string{name});
    return json::Value{std::move(object)};
}
inline SocialTaskDescriptor decode_social_task(const json::Value& value) {
    social_closed(value,{"schema","task_id","revision","performer","recipient","topic","when","location_name","location_ref","status","execution_id"});
    if(social_string(value,"schema",64)!="synth.social.task.v1")throw std::invalid_argument{"invalid social schema"};
    SocialTaskDescriptor task;task.task_id=social_string(value,"task_id",128);task.revision=social_integer(social_required(value,"revision"));
    task.performer=social_decode_actor(social_required(value,"performer"));task.recipient=social_decode_actor(social_required(value,"recipient"));
    task.topic=social_string(value,"topic",512);task.when=social_string(value,"when",32);task.location_name=social_string(value,"location_name",128,true);task.status=social_string(value,"status",32);
    const auto& a=social_required(value,"performer");const auto& b=social_required(value,"recipient");
    if(a.find("display_name"))task.performer_name=social_string(a,"display_name",255);
    if(b.find("display_name"))task.recipient_name=social_string(b,"display_name",255);
    if(const auto* ref=value.find("location_ref")) {
        social_closed(*ref,{"form_id","origin_plugin"});
        SocialLocationRef location{social_form(*ref),social_string(*ref,"origin_plugin",255)};
        if(!social_actor_valid({location.form_id,location.origin_plugin,"reference"}))throw std::invalid_argument{"invalid social location"};
        task.location_ref=std::move(location);
    }
    if(value.find("execution_id"))task.execution_id=social_string(value,"execution_id",128);
    if(!social_task_valid(task))throw std::invalid_argument{"invalid social task"};
    return task;
}
inline json::Value encode_social_task(const SocialTaskDescriptor& task) {
    if(!social_task_valid(task))throw std::invalid_argument{"invalid social task"};
    json::Value::Object object{{"schema","synth.social.task.v1"},{"task_id",task.task_id},{"revision",task.revision},
        {"performer",social_encode_actor(task.performer,task.performer_name)},{"recipient",social_encode_actor(task.recipient,task.recipient_name)},
        {"topic",task.topic},{"when",task.when},{"location_name",task.location_name},{"status",task.status}};
    if(task.location_ref) {char form[11]{};std::snprintf(form,sizeof(form),"0x%08X",task.location_ref->form_id);object.emplace_back("location_ref",json::Value{json::Value::Object{{"form_id",form},{"origin_plugin",task.location_ref->origin_plugin}}});}
    if(task.execution_id)object.emplace_back("execution_id",*task.execution_id);
    return json::Value{std::move(object)};
}
inline std::vector<std::uint8_t> encode_social_task_reminders(std::span<const SocialTaskDescriptor> tasks) {
    if(tasks.size()>8)throw std::invalid_argument{"too many social reminders"};
    json::Value::Array values;
    for(auto task:tasks){task.execution_id.reset();values.push_back(encode_social_task(task));}
    const auto text=json::write(json::Value{std::move(values)});if(text.size()>32768)throw std::invalid_argument{"social reminders too large"};
    std::vector<std::uint8_t> bytes{'S','O','C','1'};bytes.insert(bytes.end(),text.begin(),text.end());
    return bytes;
}
inline std::optional<std::vector<SocialTaskDescriptor>> decode_social_task_reminders(std::span<const std::uint8_t> bytes) noexcept try {
    if(bytes.size()<6||bytes.size()>32772||!std::equal(bytes.begin(),bytes.begin()+4,"SOC1"))return {};
    const std::string text(reinterpret_cast<const char*>(bytes.data()+4),bytes.size()-4);const auto value=json::parse(text,{32768,16,2048,8,16});
    if(!value.is_array()||value.as_array().size()>8)return {};
    std::vector<SocialTaskDescriptor> result;
    for(const auto& item:value.as_array()){auto task=decode_social_task(item);if(task.execution_id)return {};for(const auto& prior:result)if(prior.task_id==task.task_id)return {};result.push_back(std::move(task));}return result;
} catch(...){return {};}
} // namespace synth::runtime
