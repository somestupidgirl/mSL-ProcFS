/*
 * mach_assert.h
 *
 * Generated XNU build-configuration header (not part of the XNU source tree;
 * normally produced from osfmk/conf/MASTER during a kernel build). It only
 * selects whether MACH_ASSERT-style assertions are compiled in. Provided here
 * so the vendored XNU headers (kern/assert.h) compile in this kext build.
 *
 * Must be 0: with MACH_ASSERT && XNU_KERNEL_PRIVATE, kern/assert.h emits a
 * STATIC_IF_KEY_DECLARE_TRUE(mach_assert) that references the kernel-internal
 * _mach_assert_jump_key symbol, which is not exported to kexts and so fails to
 * bind at load. MACH_ASSERT 0 also matches the RELEASE kernel this kext loads
 * into.
 */
#ifndef _MACH_ASSERT_GENERATED_H_
#define _MACH_ASSERT_GENERATED_H_

#define MACH_ASSERT 0

#endif /* _MACH_ASSERT_GENERATED_H_ */
