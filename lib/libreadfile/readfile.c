/*
 *  fs.c
 *
 *  Created by Linus Henze on 11/10/2016.
 *  Copyright © 2016 Linus Henze. All rights reserved.
 */
#include <stdint.h>
#if defined(__x86_64__)
#include <i386/types.h>
#elif defined(__arm64__) || defined(__aarch64__)
#include <arm/_types.h>
#endif
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/vnode_if.h>

#include "readfile.h"

int read_file(char *file, uint8_t *buffer, off_t offset, user_size_t size) {
    int res = EIO;
    
    vfs_context_t ctx = vfs_context_create(NULL);
    if (ctx == NULL) {
        return EIO;
    }
    
    vnode_t vp = NULLVP;
    if (vnode_lookup(file, 0, &vp, ctx) == 0) {
        uio_t uio = uio_create(1, offset, UIO_SYSSPACE, UIO_READ);
        if (uio == NULL) {
            goto exit;
        }
        
        if (uio_addiov(uio, CAST_USER_ADDR_T(buffer), size)) {
            goto exit;
        }
        
        if (VNOP_READ(vp, uio, 0, ctx)) {
            goto exit;
        }
        
        if (uio_resid(uio)) {
            goto exit;
        }
        
        res = 0;
    } else {
        vfs_context_rele(ctx);
        return ENOENT;
    }
    
exit:
    vnode_put(vp);
    vfs_context_rele(ctx);
    return res;
}
