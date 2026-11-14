#pragma once

#include "core/readable_note_gate.hpp"
#include <F4SE/F4SE.h>
#include <RE/Fallout.h>
#include <array>
#include <cstring>

namespace synth::adapters {

// Exact flat 1.11.240 BookMenu open capture; static audit only, no gameplay acceptance.
// Both direct callers of the open-population function are rewritten together or not at all.
// The hook copies a BOOK form ID and bounded body on the main thread; admission happens later
// in pump() only when the menu freshly opened, is visible and still shows the same BOOK and body.
class FlatReadableNotes final {
public:
    using Read = core::ReadableNoteGate::Read;

    // Call while F4SE is single-threaded at load (trampoline already allocated).
    [[nodiscard]] static bool install() noexcept {
        if (attempted_) return installed_;
        attempted_ = true;
        const auto module = REX::FModule::GetExecutingModule();
        if (module.GetFileVersion().pack() != REL::Version{1, 11, 240, 0}.pack()) return false;
        const auto base = module.GetBaseAddress();
        const auto open = base + open_rva_;
        auto& trampoline = REL::GetTrampoline();
        if (REL::ID(open_id_).address() != open || REL::ID(world_caller_id_).address() != base + 0x465620 ||
            REL::ID(inventory_caller_id_).address() != base + 0xAE2460 ||
            REL::ID(book_static_id_).address() != base + 0x3312578 ||
            REL::ID(body_static_id_).address() != base + 0x3312590 ||
            read<Call>(base + site_rva_[0]) != original_bytes_[0] ||
            read<Call>(base + site_rva_[1]) != original_bytes_[1] || trampoline.free_size() < sizeof(Stub)) {
            REX::WARN("SYNTH readable notes unavailable: BookMenu open bindings differ from audited 1.11.240");
            return false;
        }
        // Reserve the one stub both sites share (pinned allocate_branch5 keys it by thunk) and prove
        // it is a fresh allocation of exactly the known bytes before any game code is touched.
        // allocate(0) returns the current trampoline cursor without consuming space, so the stub
        // must start exactly there and end inside the trampoline's committed capacity.
        const auto hook = reinterpret_cast<std::uintptr_t>(&open_hook);
        const auto used = trampoline.allocated_size();
        std::uintptr_t next{}, stub{};
        try {
            next = reinterpret_cast<std::uintptr_t>(trampoline.allocate(0));
            stub = trampoline.allocate_branch5(hook);
        } catch (...) {
            stub = 0;
        }
        installed_stub_ = Patch::stub(hook);
        bool reserved = next != 0 && stub == next && trampoline.allocated_size() == used + sizeof(Stub) &&
                        trampoline.allocated_size() <= trampoline.capacity() && read<Stub>(stub) == installed_stub_;
        for (std::size_t i = 0; reserved && i < site_rva_.size(); ++i) {
            const auto call = Patch::call(base + site_rva_[i], stub);
            reserved = call.has_value();
            if (call) patched_bytes_[i] = *call;
        }
        if (!reserved) {
            REX::WARN("SYNTH readable notes unavailable: trampoline stub could not be reserved exactly");
            return false;
        }
        stub_ = stub;
        original_ = reinterpret_cast<OpenFn>(open); // Set first: a kept patch still forwards once.
        for (std::size_t i = 0; i < site_rva_.size(); ++i) {
            const auto site = base + site_rva_[i];
            if (read<Call>(site) != original_bytes_[i] ||
                !REL::WriteSafe(site, patched_bytes_[i].data(), patched_bytes_[i].size()) || !owned(i, site)) {
                if (rollback(base, i + 1))
                    REX::WARN("SYNTH readable notes unavailable: BookMenu open calls were not both owned");
                else
                    REX::WARN("SYNTH readable notes unavailable: rollback incomplete, foreign BookMenu call bytes kept");
                return false;
            }
        }
        installed_ = true;
        REX::INFO("SYNTH readable notes: exact flat BookMenu open callers installed");
        return true;
    }

    // Verified owned hooks and a registered sink are both required before capture is offered.
    [[nodiscard]] static bool available() noexcept {
        if (!installed_ || !sink_registered_) return false;
        const auto base = REX::FModule::GetExecutingModule().GetBaseAddress();
        return owned(0, base + site_rva_[0]) && owned(1, base + site_rva_[1]);
    }

    // Any thread (lifecycle callbacks): scalar supersession only.
    static void invalidate() noexcept { gate_.invalidate(); }

    // Game-thread pump. Returns a copied read only once it is proven freshly visible for this generation.
    [[nodiscard]] static std::optional<Read> pump(std::uint64_t generation) noexcept {
        if (!installed_) return {};
        const auto* engine = RE::Main::GetSingleton();
        const auto thread = GetCurrentThreadId();
        if (!engine || engine->threadID == 0 || engine->threadID != thread) return {};
        main_thread_.store(thread, std::memory_order_release);
        if (generation_.exchange(generation, std::memory_order_acq_rel) != generation) gate_.invalidate();
        if (!sink_registered_) register_menu_sink();
        if (!sink_registered_) return {}; // UI not ready yet; nothing can have been staged.
        // Another hook replacing either call site or the stub ends capture for this process: the
        // pending candidate is dropped and the hook only forwards from then on.
        if (!available()) {
            installed_ = false;
            gate_.invalidate();
            gate_.drop();
            REX::WARN("SYNTH readable notes disabled: BookMenu open call or stub bytes are no longer owned");
            return {};
        }
        if (!gate_.pending()) return {};
        core::ReadableNoteGate::Display display{generation};
        if (const auto menu = book_menu()) {
            display.on_stack = menu->OnStack();
            display.display_enabled = menu->IsMenuDisplayEnabled();
        }
        // The open statics are read only while the menu that owns them is visibly on stack.
        if (display.on_stack && display.display_enabled) {
            display.book_form_id = static_book();
            display.body = bounded(REL::Relocation<RE::BSString*>{REL::ID(body_static_id_)}.get());
        }
        return gate_.admit(display, core::ReadableNoteGate::Clock::now());
    }

private:
    struct MenuSink final : RE::BSTEventSink<RE::MenuOpenCloseEvent> {
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& event,
                                              RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
            // May run off the game thread: pointer identity of the interned name and one atomic only.
            if (event.menuName.data() == book_menu_data_.load(std::memory_order_acquire))
                gate_.observe_menu(event.opening);
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    using OpenFn = void (*)(RE::BSString*, RE::ExtraDataList*, RE::TESObjectREFR*, RE::TESObjectBOOK*,
                            const void*, const void*, float, bool);

    // Verified game thread only; the interned name is set before the sink can observe anything.
    // The name is intentionally leaked so no pool release runs during DLL static teardown.
    static void register_menu_sink() noexcept {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return;
        if (!book_menu_name_) {
            try {
                book_menu_name_ = new RE::BSFixedString{"BookMenu"};
            } catch (...) {
                return;
            }
        }
        book_menu_data_.store(book_menu_name_->data(), std::memory_order_release);
        ui->RegisterSink<RE::MenuOpenCloseEvent>(&sink_);
        sink_registered_ = true;
    }

    // The attempt epoch advances before any engine access; only the cached main thread copies, and
    // staging happens after the original returned once so a nested open always wins.
    static void open_hook(RE::BSString* body, RE::ExtraDataList* extra, RE::TESObjectREFR* ref,
                          RE::TESObjectBOOK* book, const void* pos, const void* rot, float scale, bool flag) {
        const auto attempt = gate_.begin_attempt();
        const auto main = main_thread_.load(std::memory_order_acquire);
        const bool eligible = main != 0 && GetCurrentThreadId() == main && installed_ && sink_registered_;
        const auto generation = generation_.load(std::memory_order_acquire);
        std::uint32_t book_id{};
        bool was_open{true};
        std::string copied;
        if (eligible) {
            if (book && book->GetFormType() == RE::ENUM_FORM_ID::kBOOK) book_id = book->GetFormID();
            auto* ui = RE::UI::GetSingleton();
            was_open = !ui || ui->GetMenuOpen(*book_menu_name_); // No instance means not on stack.
            try {
                copied.assign(bounded(body));
            } catch (...) {
                copied.clear();
            }
        }
        original_(body, extra, ref, book, pos, rot, scale, flag);
        if (eligible && generation == generation_.load(std::memory_order_acquire))
            (void)gate_.stage(attempt, generation, was_open, book_id, copied, core::ReadableNoteGate::Clock::now());
    }

    static Scaleform::Ptr<RE::IMenu> book_menu() noexcept {
        auto* ui = RE::UI::GetSingleton();
        return ui ? ui->GetMenu(*book_menu_name_) : nullptr;
    }

    static std::uint32_t static_book() noexcept {
        const auto* book = *REL::Relocation<RE::TESObjectBOOK**>{REL::ID(book_static_id_)};
        return book && book->GetFormType() == RE::ENUM_FORM_ID::kBOOK ? book->GetFormID() : 0;
    }

    // Reads the stored length directly; the 0xFFFF strlen sentinel is treated as unbounded and rejected.
    static std::string_view bounded(const RE::BSString* text) noexcept {
        static_assert(sizeof(RE::BSString) == 0x10);
        if (!text) return {};
        const char* data{};
        std::uint16_t size{};
        std::memcpy(&data, text, sizeof(data));
        std::memcpy(&size, reinterpret_cast<const std::uint8_t*>(text) + 8, sizeof(size));
        if (!data || size == 0xFFFF || size > core::ReadableNoteGate::max_body_bytes) return {};
        return {data, size};
    }

    using Patch = core::ReadableNotePatch;
    using Call = Patch::Call;
    using Stub = Patch::Stub;
    static_assert(sizeof(Stub) == sizeof(REL::ASM::JMP14));

    template <class Bytes>
    static Bytes read(std::uintptr_t address) noexcept {
        Bytes bytes{};
        std::memcpy(bytes.data(), reinterpret_cast<const void*>(address), bytes.size());
        return bytes;
    }

    // Compares only known addresses: the site's complete recorded call, then our complete stub.
    static bool owned(std::size_t index, std::uintptr_t site) noexcept {
        return stub_ && Patch::owned(read<Call>(site), patched_bytes_[index], read<Stub>(stub_), installed_stub_);
    }

    // Restores the first count sites only where our exact call and stub remain; reports false
    // when any site holds foreign bytes or a restore did not read back as the original.
    static bool rollback(std::uintptr_t base, std::size_t count) noexcept {
        bool complete = true;
        for (std::size_t k = 0; k < count; ++k) {
            const auto site = base + site_rva_[k];
            switch (Patch::restore(read<Call>(site), original_bytes_[k], patched_bytes_[k], read<Stub>(stub_),
                                   installed_stub_)) {
            case Patch::Restore::already_original: break;
            case Patch::Restore::foreign: complete = false; break;
            case Patch::Restore::write_original:
                complete = REL::WriteSafe(site, original_bytes_[k].data(), original_bytes_[k].size()) &&
                           read<Call>(site) == original_bytes_[k] && complete;
                break;
            }
        }
        return complete;
    }

    static constexpr std::uint64_t open_id_{2248510}, world_caller_id_{2198604}, inventory_caller_id_{2224168};
    static constexpr std::uint64_t book_static_id_{2701336}, body_static_id_{2701339};
    static constexpr std::uintptr_t open_rva_{0x1033E80};
    // World (0x4657F0) and inventory (0xAE252B) callers; origin is documented, not admitted.
    static constexpr std::array<std::uintptr_t, 2> site_rva_{0x4657F0, 0xAE252B};
    static constexpr std::array<Call, 2> original_bytes_{Call{0xE8, 0x8B, 0xE6, 0xBC, 0x00},
                                                         Call{0xE8, 0x50, 0x19, 0x55, 0x00}};
    static inline std::array<Call, 2> patched_bytes_{};
    static inline Stub installed_stub_{};
    static inline std::uintptr_t stub_{};
    static inline core::ReadableNoteGate gate_;
    static inline MenuSink sink_;
    static inline RE::BSFixedString* book_menu_name_{};
    static inline std::atomic<const char*> book_menu_data_{};
    static inline OpenFn original_{};
    static inline std::atomic_uint64_t generation_{};
    static inline std::atomic_uint32_t main_thread_{};
    static inline bool attempted_{}, installed_{}, sink_registered_{};
};

} // namespace synth::adapters
