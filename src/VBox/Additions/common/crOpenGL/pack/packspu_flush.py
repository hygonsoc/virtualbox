# Copyright (c) 2001, Stanford University
# All rights reserved.
#
# See the file LICENSE.txt for information on redistributing this software.

from __future__ import print_function
import sys

import apiutil


apiutil.CopyrightC()

print("""
/* DO NOT EDIT - this file generated by packspu_flush.py script */

/* These are otherwise ordinary functions which require that the buffer be
 * flushed immediately after packing the function.
 */
#include "cr_glstate.h"
#include "cr_packfunctions.h"
#include "packspu.h"
#include "packspu_proto.h"
""")

keys = apiutil.GetDispatchedFunctions(sys.argv[1]+"/APIspec.txt")

for func_name in apiutil.AllSpecials( "packspu_flush" ):
    params = apiutil.Parameters(func_name)
    print('void PACKSPU_APIENTRY packspu_%s( %s )' % ( func_name, apiutil.MakeDeclarationString(params)))
    print('{')
    print('\tGET_THREAD(thread);')
    print('\tif (pack_spu.swap)')
    print('\t{')
    print('\t\tcrPack%sSWAP( %s );' % ( func_name, apiutil.MakeCallString( params ) ))
    print('\t}')
    print('\telse')
    print('\t{')
    print('\t\tcrPack%s( %s );' % ( func_name, apiutil.MakeCallString( params ) ))
    print('\t}')
    print('\tpackspuFlush( (void *) thread );')
    print('}\n')
