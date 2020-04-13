# coding: utf-8

# Copyright (C) 1994-2020 Altair Engineering, Inc.
# For more information, contact Altair at www.altair.com.
#
# This file is part of the PBS Professional ("PBS Pro") software.
#
# Open Source License Information:
#
# PBS Pro is free software. You can redistribute it and/or modify it under the
# terms of the GNU Affero General Public License as published by the Free
# Software Foundation, either version 3 of the License, or (at your option) any
# later version.
#
# PBS Pro is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.
# See the GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
# Commercial License Information:
#
# For a copy of the commercial license terms and conditions,
# go to: (http://www.pbspro.com/UserArea/agreement.html)
# or contact the Altair Legal Department.
#
# Altair’s dual-license business model allows companies, individuals, and
# organizations to create proprietary derivative works of PBS Pro and
# distribute them - whether embedded or bundled with other software -
# under a commercial license agreement.
#
# Use of Altair’s trademarks, including but not limited to "PBS™",
# "PBS Professional®", and "PBS Pro™" and Altair’s logos is subject to Altair's
# trademark licensing policies.

from tests.functional import *


class TestSchedRunjobWait(TestFunctional):
    """
    Tests related to scheduler attribute runjob_wait
    """

    def setup_scn(self, n):
        """
        set up n multi-scheds for a test
        """
        sc_quenames = []
        for i in range(n):
            scname = "sc" + str(i)
            pname = "P" + str(i)
            portnum = str(15050 + i)
            qname = "wq" + str(i)
            sc_quenames.append([scname, qname])

            a = {'partition': pname,
                 'sched_host': self.server.hostname,
                 'sched_port': portnum}
            self.server.manager(MGR_CMD_CREATE, SCHED,
                                a, id=scname)
            self.scheds[scname].create_scheduler()
            self.scheds[scname].start()
            self.server.manager(MGR_CMD_SET, SCHED,
                                {'scheduling': 'True'}, id=scname)
            self.server.manager(MGR_CMD_SET, SCHED,
                                {'log_events': 2047}, id=scname)

            a = {'queue_type': 'execution',
                 'started': 'True',
                 'enabled': 'True'}
            self.server.manager(MGR_CMD_CREATE, QUEUE, a, id=qname)
            p = {'partition': pname}
            self.server.manager(MGR_CMD_SET, QUEUE, p, id=qname)
            a = {'resources_available.ncpus': 1}
            prefix = 'vnode' + str(i)
            nname = prefix + "[0]"
            self.server.create_vnodes(prefix, a, 1, self.mom,
                                      delall=False, additive=True)
            self.server.manager(MGR_CMD_SET, NODE, p, id=nname)

        return sc_quenames

    def test_throughput_mode_deprecated(self):
        """
        Test that server logs throughput_mode as deprecated
        """
        t1 = time.time()
        self.server.manager(MGR_CMD_SET, SCHED,
                            {'throughput_mode': "True"}, id="default")
        msg = "'throughput_mode' is being deprecated, " +\
            "it is recommended to use 'runjob_wait' in future"
        self.server.log_match(msg, starttime=t1)

    def test_runjobwait_throughput_clash(self):
        """
        Test that runjob_wait and throughput_mode cannot both be set
        """
        errmsg = "Setting both throughput_mode and runjob_wait not allowed"
        self.server.manager(MGR_CMD_SET, SCHED,
                            {'throughput_mode': "True"}, id="default")
        try:
            self.server.manager(MGR_CMD_SET, SCHED,
                                {'runjob_wait': "none"}, id="default")
            self.fail("Throughput_mode and runjob_wait were both set!")
        except PbsManagerError as e:
            self.assertIn(errmsg, str(e))

        self.server.manager(MGR_CMD_UNSET, SCHED,
                            'throughput_mode', id="default")

        self.server.manager(MGR_CMD_SET, SCHED,
                            {'runjob_wait': "none"}, id="default")
        try:
            self.server.manager(MGR_CMD_SET, SCHED,
                                {'throughput_mode': "True"}, id="default")
            self.fail("Throughput_mode and runjob_wait were both set!")
        except PbsManagerError as e:
            self.assertIn(errmsg, str(e))

    def test_runjobwait_default(self):
        """
        Test that runjob wait gets set to its default when unset
        """
        try:
            self.server.manager(MGR_CMD_UNSET, SCHED,
                                'runjob_wait', id="default")
        except PbsManagerError:
            pass

        # Check that it's set to the default
        self.server.expect(SCHED, {'runjob_wait': 'runjob_hook'}, id='default')

    def test_valid_vals(self):
        """
        Test that runjob_wait can only be set to its default values
        """
        self.server.manager(MGR_CMD_SET, SCHED, {'runjob_wait': 'none'},
                            id='default')
        self.server.manager(MGR_CMD_SET, SCHED,
                            {'runjob_wait': 'runjob_hook'}, id='default')
        self.server.manager(MGR_CMD_SET, SCHED,
                            {'runjob_wait': 'execjob_hook'}, id='default')
        try:
            self.server.manager(MGR_CMD_SET, SCHED,
                                {'runjob_wait': 'badstr'}, id='default')
            self.fail("invalid str value for runjob_wait was accepted")
        except PbsManagerError:
            pass

        try:
            self.server.manager(MGR_CMD_SET, SCHED,
                                {'runjob_wait': 0}, id='default')
            self.fail("invalid int value for runjob_wait was accepted")
        except PbsManagerError:
            pass

    def test_multisched_multival(self):
        """
        Test that multiple scheds can be configured with different vals of
        runjob_wait, and behave correctly
        """
        sc_queue = self.setup_scn(3)
        a = {"scheduling": "False", "runjob_wait": "none"}
        self.server.manager(MGR_CMD_SET, SCHED, a, id=sc_queue[0][0])
        a["runjob_wait"] = "runjob_hook"
        self.server.manager(MGR_CMD_SET, SCHED, a, id=sc_queue[1][0])
        a["runjob_wait"] = "execjob_hook"
        self.server.manager(MGR_CMD_SET, SCHED, a, id=sc_queue[2][0])

        hook_txt = """
import pbs

if pbs.event().job.id == '%s':
    pbs.event().reject("rejecting first job")
pbs.event().accept()
"""
        hk_attrs = {'event': 'runjob', 'enabled': 'True'}

        # All of the scheds have a 1 ncpu node only
        # Submit 2 1cpu jobs to each sched
        # The runjob hook will reject first job that's run by each sched
        a = {"queue": sc_queue[0][1], "Resource_List.ncpus": "1"}
        jid1 = self.server.submit(Job(attrs=a))
        jid2 = self.server.submit(Job(attrs=a))
        self.server.create_import_hook('rj', hk_attrs, hook_txt % (jid1))

        # sched 1 with runjob_wait=none runs first job without waiting
        # for runjob reject, so it doesn't run second job.
        # Ultimately, neither jobs should run
        self.scheds[sc_queue[0][0]].run_scheduling_cycle()
        self.server.expect(JOB, {'job_state': 'Q'}, id=jid1)
        self.server.expect(JOB, {'job_state': 'Q'}, id=jid2)

        self.server.delete_hook('rj')
        a["queue"] = sc_queue[1][1]
        jid3 = self.server.submit(Job(attrs=a))
        jid4 = self.server.submit(Job(attrs=a))
        self.server.create_import_hook('rj', hk_attrs, hook_txt % str(jid3))

        # sched 2 with runjob_wait=runjob_hook should wait for runjob
        # reject and then run the second job
        self.scheds[sc_queue[1][0]].run_scheduling_cycle()
        self.server.expect(JOB, {'job_state': 'Q'}, id=jid3)
        self.server.expect(JOB, {'job_state': 'R'}, id=jid4)

        self.server.delete_hook('rj')
        a["queue"] = sc_queue[1][1]
        jid5 = self.server.submit(Job(attrs=a))
        jid6 = self.server.submit(Job(attrs=a))
        hk_attrs["event"] = 'execjob_begin'
        self.server.create_import_hook('ej', hk_attrs, hook_txt % str(jid5))

        # sched 2 with runjob_wait=runjob_hook won't wait for execjob_begin
        # reject, so it will run first job and not run second.
        # Ultimately no jobs will run
        self.scheds[sc_queue[1][0]].run_scheduling_cycle()
        self.server.expect(JOB, {'job_state': 'Q'}, id=jid5)
        self.server.expect(JOB, {'job_state': 'Q'}, id=jid6)

        self.server.delete_hook('ej')
        a["queue"] = sc_queue[2][1]
        jid7 = self.server.submit(Job(attrs=a))
        jid8 = self.server.submit(Job(attrs=a))
        hk_attrs["event"] = 'execjob_begin'
        self.server.create_import_hook('ej', hk_attrs, hook_txt % str(jid7))

        # sched 3 with runjob_wait=execjob_hook should wait for runjob
        # reject and then run the second job
        self.scheds[sc_queue[2][0]].run_scheduling_cycle()
        self.server.expect(JOB, {'job_state': 'Q'}, id=jid7)
        self.server.expect(JOB, {'job_state': 'R'}, id=jid8)

    def test_no_runjob_hook(self):
        """
        Test that when there is no runjob hook configured, sched behaves as if
        runjob_wait == none, even if it's set to "runjob_hook"
        """

        a = {"scheduling": "False", "runjob_wait": "runjob_hook"}
        self.server.manager(MGR_CMD_SET, SCHED, a, id="default")

        self.server.submit(Job())

        t = time.time()
        self.scheduler.run_scheduling_cycle()

        # Check that sched was able to access hooks data
        logmsg = "is unauthorized to access hooks data from server"
        self.server.log_match(logmsg, starttime=t, existence=False)

        # Check that server received PBS_BATCH_AsyrunJob, truly async request
        logmsg = "Type 23 request received"
        self.server.log_match(logmsg, starttime=t)

    def test_with_runjob_hook(self):
        """
        Test that when there is a runjob hook configured, sched doesn't
        upgrade runjob_wait from "runjob_hook" to "none"
        """

        a = {"scheduling": "False", "runjob_wait": "runjob_hook"}
        self.server.manager(MGR_CMD_SET, SCHED, a, id="default")

        hook_txt = """
import pbs

pbs.event().accept()
"""
        hk_attrs = {'event': 'runjob', 'enabled': 'True'}
        self.server.create_import_hook('rj', hk_attrs, hook_txt)

        self.server.submit(Job())

        t = time.time()
        self.scheduler.run_scheduling_cycle()

        # Check that sched was able to access hooks data
        logmsg = "is unauthorized to access hooks data from server"
        self.server.log_match(logmsg, starttime=t, existence=False)

        # Check that server received PBS_BATCH_AsyrunJob_ack request
        self.server.log_match("Type 97 request received", starttime=t)

    def test_throughput_ok(self):
        """
        Test that throughput_mode still works correctly
        """
        self.server.manager(MGR_CMD_UNSET, SCHED,
                            'runjob_wait', id="default")

        a = {'throughput_mode': "True", "scheduling": "False"}
        self.server.manager(MGR_CMD_SET, SCHED, a, id="default")

        jid = self.server.submit(Job())

        t = time.time()
        self.scheduler.run_scheduling_cycle()
        self.server.expect(JOB, {"job_state": "R"}, id=jid)

        # Check that server received PBS_BATCH_AsyrunJob_ack request
        self.server.log_match("Type 97 request received", starttime=t)

        self.server.manager(MGR_CMD_SET, SCHED, {'throughput_mode': "False"},
                            id="default")

        jid = self.server.submit(Job())
        t = time.time()
        self.scheduler.run_scheduling_cycle()
        self.server.expect(JOB, {"job_state": "R"}, id=jid)

        # Check that server received PBS_BATCH_RunJob request
        self.server.log_match("Type 15 request received", starttime=t)
