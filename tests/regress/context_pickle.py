import pickle
import sys

from unicorn import Uc, UC_ARCH_X86, UC_MODE_64
from unicorn.x86_const import UC_X86_REG_RAX, UC_X86_REG_XMM0


mu = Uc(UC_ARCH_X86, UC_MODE_64)
context = mu.context_save()
context_state = context.__getstate__()
assert len(context_state[0]) == context_state[1]
context = pickle.loads(pickle.dumps(context))

if sys.version_info[0] == 2:
    import new

    malformed_context = new.instance(context.__class__)
    try:
        malformed_context.__setstate__((
            context_state[0][:-1],
            context_state[1],
            context_state[2],
            context_state[3],
        ))
    except ValueError:
        pass
    else:
        raise AssertionError("Malformed Unicorn context was accepted")

    assert malformed_context._to_free is False
    del malformed_context

rax = 0x0123456789ABCDEF
xmm0 = 0xFFEEDDCCBBAA99887766554433221100
context.reg_write(UC_X86_REG_RAX, rax)
context.reg_write(UC_X86_REG_XMM0, xmm0)

assert context.reg_read(UC_X86_REG_RAX) == rax
assert context.reg_read(UC_X86_REG_XMM0) == xmm0
assert context.reg_read_batch((
    UC_X86_REG_RAX,
    UC_X86_REG_XMM0,
)) == (rax, xmm0)

rax += 1
xmm0 += 1
context.reg_write_batch(((UC_X86_REG_RAX, rax), (UC_X86_REG_XMM0, xmm0)))

mu.context_restore(context)
assert mu.reg_read(UC_X86_REG_RAX) == rax
assert mu.reg_read(UC_X86_REG_XMM0) == xmm0
