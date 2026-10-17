import copy
import unittest
import script_gate

class ScriptGateTests(unittest.TestCase):
    def log(self):
        lines=[]
        for i,state in enumerate(script_gate.STATES):
            play=state in ('reopened-play','floor-contact','reset','second-play')
            bodies=7 if state.startswith('sandbox-') else 3
            force='[4,0,0]' if state in ('reopened-play','reset','edit-restored') else 'none'
            lines.append(f'SCRIPT state={state} frame={i*40} epoch={i+1} bodies={bodies} scripts={int(play)} x=1 y=0.5 force={force}')
        lines.append('SCRIPT input PASS: SDL repeats, capture, focus and Play transitions')
        lines.append('SCRIPT lifecycle PASS: scratch edit/save/reopen/run, force/floor, reset, leave, sandbox bodies=7 scripts=0')
        return '\n'.join(lines)
    def test_complete_realistic_transitions(self):
        self.assertEqual(len(script_gate.check(self.log())),7)
    def test_missing_or_repeated_state_is_rejected(self):
        log=self.log()
        for bad in (log.replace('SCRIPT state=reset','SKIPPED state=reset'),log+'\n'+log.splitlines()[0]):
            with self.assertRaises(AssertionError):script_gate.check(bad)
    def test_bad_force_floor_count_or_epoch_is_rejected(self):
        log=self.log()
        for bad in (log.replace('force=[4,0,0]','force=[1.5,0,0]',1),log.replace(' x=1 y=0.5',' x=0 y=-2'),
                    log.replace('bodies=3 scripts=1','bodies=3 scripts=0',1),log.replace('epoch=4','epoch=1')):
            with self.assertRaises(AssertionError):script_gate.check(bad)
    def test_missing_terminal_pass_is_rejected(self):
        for marker in ('SCRIPT lifecycle PASS:','SCRIPT input PASS:'):
            with self.assertRaises(AssertionError):script_gate.check(self.log().replace(marker,'MISSING:'))

if __name__=='__main__':unittest.main()

