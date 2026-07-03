/*
 *  fs.c
 *
 *  Created by Linus Henze on 11/10/2016.
 *  Copyright © 2016 Linus Henze. All rights reserved.
 */

#ifndef fs_h
#define fs_h

#include <sys/vnode.h>

int read_file(char *file, uint8_t *buffer, off_t offset, user_size_t size);

#endif /* fs_h */
