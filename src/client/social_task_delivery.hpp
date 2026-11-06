#pragma once

#include "protocol_native/v1_codec.hpp"
#include "presentation/dialogue_presentation.hpp"
#include "runtime/social_task.hpp"
#include <cstdio>

namespace synth::client {

inline bool social_wire_actor_matches(const protocol_native::Identity& wire,const runtime::RuntimeActorIdentity& actor) {
    char form[11]{};std::snprintf(form,sizeof(form),"0x%08X",actor.form_id);
    return runtime::social_plugin_equal(wire.form_id,form)&&runtime::social_plugin_equal(wire.origin_plugin,actor.origin_plugin)&&wire.playthrough_id==actor.playthrough_id;
}

// A server line is only evidence after its exact recipient reply was presented.
// Request ancestry is copied from the owned arrived request and rechat parents.
class SocialTaskDelivery final {
public:
    struct Proof final { std::string task_id, execution_id, request_id, line_id; };
    void begin(const runtime::SocialTaskCommand& command, std::uint64_t generation) {
        task_=command.task;execution_=command.execution_id;generation_=generation;
        requests_.clear();caption_.reset();proof_.reset();
    }
    void clear() noexcept {task_.reset();requests_.clear();caption_.reset();proof_.reset();execution_.clear();generation_=0;}
    void root_request(std::string_view request) {if(task_&&runtime::social_id(request))add_request(request);}
    void child_request(std::string_view parent,std::string_view request) {
        if(known(parent)&&runtime::social_id(request))add_request(request);
    }
    void complete_request(std::string_view request) {
        for(auto& value:requests_)if(value.id==request)value.complete=true;
    }
    [[nodiscard]] bool owns_request(std::string_view request) const {return known(request);}
    bool track(const protocol_native::Line& line,const presentation::DialogueCaptions::Caption& caption) {
        if(!task_||!caption||line.generation!=generation_||!known(line.request_id)||
            caption->generation!=line.generation||caption->request_id!=line.request_id||
            caption->line_id!=line.line_id||caption->context_sequence!=line.context_sequence)return false;
        const auto* dialogue=std::get_if<protocol_native::Dialogue>(&line.payload);
        if(!dialogue||!dialogue->listener)return false;
        const auto* speaker=std::get_if<protocol_native::Identity>(&dialogue->speaker);
        if(!speaker||!social_wire_actor_matches(*speaker,task_->recipient)||!social_wire_actor_matches(*dialogue->listener,task_->performer))return false;
        if(!runtime::social_id(line.request_id)||!runtime::social_id(line.line_id))return false;
        caption_=caption;proof_=Proof{task_->task_id,execution_,line.request_id,line.line_id};
        return true;
    }
    [[nodiscard]] std::optional<Proof> delivered_proof() const {
        if(!proof_||!caption_||caption_->cancellation.is_cancelled()||!caption_->delivered())return {};
        for(const auto& value:requests_)if(value.id==proof_->request_id&&value.complete)return proof_;
        return {};
    }
private:
    struct Request final {std::string id;bool complete{};};
    [[nodiscard]] bool known(std::string_view request) const {
        return std::ranges::any_of(requests_,[&](const auto& value){return value.id==request;});
    }
    void add_request(std::string_view request) {
        if(!known(request)&&requests_.size()<16)requests_.push_back({std::string{request},false});
    }
    std::optional<runtime::SocialTaskDescriptor> task_;
    std::string execution_;std::uint64_t generation_{};
    std::vector<Request> requests_;
    presentation::DialogueCaptions::Caption caption_;
    std::optional<Proof> proof_;
};
} // namespace synth::client
