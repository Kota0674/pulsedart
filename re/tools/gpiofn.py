"""Classify nrf_gpio inline copies (0x5c700-0x5ce68) as CFG/SET/CLR/READ/OUTREAD/DIR."""
import struct
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
img=open('dump/flash.bin','rb').read()
md=Cs(CS_ARCH_ARM, CS_MODE_THUMB)
def classify(lo=0x5c700, hi=0x5ce68):
    res={}
    insns=list(md.disasm(img[lo:hi],lo))
    starts=[i.address for i in insns if i.mnemonic.startswith('push')]
    for k,s in enumerate(starts):
        e=starts[k+1] if k+1<len(starts) else hi
        body=[i for i in insns if s<=i.address<e]
        txt=' | '.join(i.mnemonic+' '+i.op_str for i in body)
        tag=None
        if '#0x700' in txt:
            tag='gpio_cfg'
        elif '#0x508]' in txt and 'str' in txt: tag='gpio_set'
        elif '#0x50c]' in txt and 'str' in txt: tag='gpio_clr'
        elif '#0x510]' in txt: tag='gpio_read'
        elif '#0x504]' in txt: tag='gpio_outread/write'
        elif '#0x518]' in txt or '#0x51c]' in txt: tag='gpio_dir'
        if tag: res[s]=tag
    return res
if __name__=='__main__':
    for a,t in sorted(classify().items()): print(hex(a),t)
