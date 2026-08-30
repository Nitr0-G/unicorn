package unicorn

import "runtime"

// #include <unicorn/unicorn.h>
import "C"

type Context **C.uc_context

func (u *uc) ContextSave(reuse Context) (Context, error) {
	ctx := reuse
	if ctx == nil {
		ctx = new(*C.uc_context)
		ucerr := C.uc_context_alloc(u.handle, ctx)
		runtime.KeepAlive(u)
		runtime.KeepAlive(ctx)
		if err := errReturn(ucerr); err != nil {
			return nil, err
		}
	}
	ucerr := C.uc_context_save(u.handle, *ctx)
	runtime.KeepAlive(u)
	runtime.KeepAlive(ctx)
	if err := errReturn(ucerr); err != nil {
		if reuse == nil {
			C.uc_context_free(*ctx)
			runtime.KeepAlive(u)
			runtime.KeepAlive(ctx)
		}
		return nil, err
	}
	if reuse == nil {
		runtime.SetFinalizer(ctx, func(p Context) { C.uc_context_free(*p) })
	}
	return ctx, nil
}

func (u *uc) ContextRestore(ctx Context) error {
	ucerr := C.uc_context_restore(u.handle, *ctx)
	runtime.KeepAlive(u)
	runtime.KeepAlive(ctx)
	return errReturn(ucerr)
}
