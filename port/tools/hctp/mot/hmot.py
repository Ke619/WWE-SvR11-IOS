"""HCTP (PS2) motion records -> poses: tools/hctp_motion.py, one copy (this folder's scripts import it as hmot)."""
import os, sys
sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
import hctp_motion

sys.modules[__name__] = hctp_motion
