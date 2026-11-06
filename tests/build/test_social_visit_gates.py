"""Native-only navigation safety gates; engine behavior still needs flat gameplay."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SocialVisitGates(unittest.TestCase):
    def test_native_arrival_requires_active_owned_package_not_proximity_alone(self):
        source = (ROOT / 'src/adapters/flat_social_visit.hpp').read_text()
        self.assertIn('lease_->installed && live.expected && live.package==live.expected && live.distance<=256', source)
        self.assertIn('!plugin_equal(origin_plugin(*actor),request.actor.origin_plugin)', source)
        self.assertIn('FlatWaitPackage::explicit_wait_owned(request.actor.form_id)', source)
        self.assertIn('value->lifeState!=0', source)
        self.assertIn('value->IsInCombat()', source)
        self.assertIn('std::min(request.deadline,now+std::chrono::seconds{120})', source)
        approach = (ROOT / 'src/adapters/flat_conversation_approach.hpp').read_text()
        proof = approach.split('static bool navigation_observed(', 1)[1].split('public:', 1)[0]
        self.assertIn('active && active->form_id==expected->GetFormID()', proof)
        self.assertNotIn('distance<=', proof)

    def test_vm_timeout_retains_stack_until_matching_finish_and_recovers_old_callbacks(self):
        source = (ROOT / 'src/adapters/flat_social_visit.hpp').read_text()
        callback = source.split('static void finished(', 1)[1].split('static void observed(', 1)[0]
        self.assertIn('job_->ticket==ticket', callback)
        self.assertIn('else recovery_requested_=true', callback)
        self.assertNotIn('job_.reset()', callback)
        timeout = source.split('bool draining{},warn{};', 1)[1].split('if (completed) {', 1)[0]
        self.assertIn('if (!job_->receipt.done())', timeout)
        self.assertIn('job_->receipt.revoke_install()', timeout)
        self.assertIn('completed=std::move(job_);job_.reset()', timeout)
        self.assertIn('if (draining)', timeout)
        self.assertIn('return;', timeout)
        self.assertIn('ResetVisitByFormId', source)

    def test_pre_acquisition_cancel_requires_empty_owned_alias_recovery(self):
        source = (ROOT / 'src/adapters/flat_social_visit.hpp').read_text()
        release = source.split('if (request.release) {', 1)[1].split('if (menu_mode_active())', 1)[0]
        self.assertIn('same_execution(*last_request_,request)', release)
        self.assertIn('!initialized_ || job_ || recovery_requested_', release)
        self.assertIn('Status::pending,"alias_recovery"', release)
        self.assertIn('Status::released,"no_owned_visit"', release)

    def test_visit_pump_and_load_recovery_are_independent_of_dialogue(self):
        source = (ROOT / 'src/flat/plugin_entry.cpp').read_text()
        retirement = source.split('void retire_session(', 1)[1].split('void invalidate_sessions(', 1)[0]
        self.assertLess(retirement.index('FlatSocialVisit::cancel_all()'), retirement.index('if (!session) return;'))
        pump = source.split('void pump_runtime_impl()', 1)[1].split('void pump_runtime() noexcept', 1)[0]
        cleanup = pump.index('FlatSocialVisit::tick(now)')
        self.assertLess(pump.index('world_ready_for_capture()'), cleanup)
        self.assertLess(cleanup, pump.index('adopt_session_result(now)'))
        self.assertIn('FlatSocialVisit::retire()', source)
        self.assertIn('FlatSocialVisit::bind(*vm)', (ROOT / 'src/adapters/commonlib_papyrus_api.hpp').read_text())


if __name__ == '__main__':
    unittest.main()
