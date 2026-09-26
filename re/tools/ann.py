"""Annotated disassembly: names BL targets (GPIO helpers etc.) and shows pin args.
usage: python re/tools/ann.py START END"""
import struct, sys
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
from capstone.arm import ARM_OP_MEM, ARM_REG_PC
sys.path.insert(0,'re/tools')
from gpiofn import classify
img=open('dump/flash.bin','rb').read()
N=classify()
for a in (0x5c948,0x5c962,0x5c97c,0x5c996,0x5c9b0): N[a]='gpio_cfg_input(pin,pull)'
for a in (0x5c9ca,0x5ca66,0x5ca06,0x5ca1e,0x5ca36,0x5ca4e): N[a]='gpio_cfg_output'
for a in (0x5c8b4,0x5c918,0x5c930,0x5c8cc): N[a]='gpio_cfg_default'
N[0x5caa2]='gpio_cfg_sense_input(pin,pull,sense)'
N.update({0x5c880:'gpio_cfg(pin,dir,inp,pull,drive,sense)',0x5c744:'gpio_cfg_raw(pin,dir,inp,pull,drive,sense)',
 0x5044a:'memcpy',0x50c40:'eep50_xfer(addr16,buf,len,is_write)',0x50d10:'save_pairing',0x50e2c:'gauge_check_config',
 0x57878:'gauge_control(sub)',0x57824:'gauge_read16(reg)',0x57f40:'gauge_read16_g(reg)',0x57f54:'gauge_control_g(sub)',
 0x578c0:'gauge_control_status',0x57e5c:'gauge_read_0x3a',0x57f20:'gauge_read_flags(0x06)',0x536b4:'gauge_read_SOC(0x1c)',
 0x53700:'gauge_read_voltage(0x04)',0x5f044:'i2c_init',0x5f11c:'i2c_uninit',0x5f09c:'i2c_write_read',0x5f150:'i2c_write',
 0x5da78:'delay_us?',0x5daa8:'delay_us2?',0x5b19c:'delay_ms?'})
def pin(v): return f'P{v>>5}.{v&31:02d}' if v<48 else str(v)
s=int(sys.argv[1],16); e=int(sys.argv[2],16)
md=Cs(CS_ARCH_ARM, CS_MODE_THUMB); md.detail=True; md.skipdata=True
last={}
for i in md.disasm(img[s:e], s):
    note=''
    try:
        for op in i.operands:
            if op.type==ARM_OP_MEM and op.mem.base==ARM_REG_PC:
                a=((i.address+4)&~3)+op.mem.disp
                note=f'   ; =0x{struct.unpack_from("<I",img,a)[0]:08x}'
    except Exception: pass
    if i.mnemonic in ('movs','mov.w','movw') and i.op_str.startswith('r0, #'):
        try: last['r0']=int(i.op_str.split('#')[1],0)
        except: pass
    if i.mnemonic=='bl':
        t=int(i.op_str.lstrip('#'),16)
        nm=N.get(t)
        if nm:
            note=f'   ; {nm}'
            if nm.startswith('gpio') and 'r0' in last: note+=f' {pin(last["r0"])}'
        last={}
    print(f'{i.address:05x}: {i.mnemonic:8} {i.op_str}{note}')
