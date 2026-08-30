package unicorn

import (
	"testing"
)

func TestContext(t *testing.T) {
	u, err := NewUnicorn(ARCH_X86, MODE_32)
	if err != nil {
		t.Fatal(err)
	}
	u.RegWrite(X86_REG_EBP, 100)
	ctx, err := u.ContextSave(nil)
	if err != nil {
		t.Fatal(err)
	}
	u.RegWrite(X86_REG_EBP, 200)
	err = u.ContextRestore(ctx)
	if err != nil {
		t.Fatal(err)
	}
	val, _ := u.RegRead(X86_REG_EBP)
	if val != 100 {
		t.Fatal("context restore failed")
	}
}

func TestContextReuse(t *testing.T) {
	u, err := NewUnicorn(ARCH_X86, MODE_32)
	if err != nil {
		t.Fatal(err)
	}
	if err := u.RegWrite(X86_REG_EBP, 100); err != nil {
		t.Fatal(err)
	}
	ctx, err := u.ContextSave(nil)
	if err != nil {
		t.Fatal(err)
	}
	nativeCtx := *ctx

	if err := u.RegWrite(X86_REG_EBP, 200); err != nil {
		t.Fatal(err)
	}
	reused, err := u.ContextSave(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if reused != ctx {
		t.Fatal("context reuse returned a different Go context")
	}
	if *reused != nativeCtx {
		t.Fatal("context reuse replaced the native context")
	}

	if err := u.RegWrite(X86_REG_EBP, 300); err != nil {
		t.Fatal(err)
	}
	if err := u.ContextRestore(reused); err != nil {
		t.Fatal(err)
	}
	val, err := u.RegRead(X86_REG_EBP)
	if err != nil {
		t.Fatal(err)
	}
	if val != 200 {
		t.Fatal("context reuse did not update the saved state")
	}
}
