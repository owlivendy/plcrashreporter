/*
 * Author: Mike Ash <mikeash@plausiblelabs.com>
 *
 * Copyright (c) 2012 Plausible Labs Cooperative, Inc.
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "PLCrashAsyncObjCSection.h"


static char * const kObjCSegmentName = "__OBJC";
static char * const kDataSegmentName = "__DATA";

static char * const kObjCModuleInfoSectionName = "__module_info";
static char * const kClassListSectionName = "__objc_classlist";

struct pl_objc1_module {
    uint32_t version;
    uint32_t size;
    uint32_t name;
    uint32_t symtab;
};

struct pl_objc1_symtab {
    uint32_t sel_ref_cnt;
    uint32_t refs;
    uint16_t cls_def_count;
    uint16_t cat_def_count;
};

struct pl_objc1_class {
    uint32_t isa;
    uint32_t super;
    uint32_t name;
    uint32_t version;
    uint32_t info;
    uint32_t instance_size;
    uint32_t ivars;
    uint32_t methods;
    uint32_t cache;
    uint32_t protocols;
};

struct pl_objc1_method_list {
    uint32_t obsolete;
    uint32_t count;
};

struct pl_objc1_method {
    uint32_t name;
    uint32_t types;
    uint32_t imp;
};

struct pl_objc2_class_32 {
    uint32_t isa;
    uint32_t superclass;
    uint32_t cache;
    uint32_t vtable;
    uint32_t data_rw;
};

struct pl_objc2_class_64 {
    uint64_t isa;
    uint64_t superclass;
    uint64_t cache;
    uint64_t vtable;
    uint64_t data_rw;
};

struct pl_objc2_class_data_rw_32 {
    uint32_t flags;
    uint32_t version;
    uint32_t data_ro;
};

struct pl_objc2_class_data_rw_64 {
    uint32_t flags;
    uint32_t version;
    uint64_t data_ro;
};

struct pl_objc2_class_data_ro_32 {
    uint32_t flags;
    uint32_t instanceStart;
    uint32_t instanceSize;
    uint32_t ivarLayout;
    uint32_t name;
    uint32_t baseMethods;
    uint32_t baseProtocols;
    uint32_t ivars;
    uint32_t weakIvarLayout;
    uint32_t baseProperties;
};

struct pl_objc2_class_data_ro_64 {
    uint32_t flags;
    uint32_t instanceStart;
    uint32_t instanceSize;
    uint32_t reserved;
    uint64_t ivarLayout;
    uint64_t name;
    uint64_t baseMethods;
    uint64_t baseProtocols;
    uint64_t ivars;
    uint64_t weakIvarLayout;
    uint64_t baseProperties;
};

struct pl_objc2_method_32 {
    uint32_t name;
    uint32_t types;
    uint32_t imp;
};

struct pl_objc2_method_64 {
    uint64_t name;
    uint64_t types;
    uint64_t imp;
};

struct pl_objc2_list_header {
    uint32_t entsize;
    uint32_t count;
};


static plcrash_error_t read_string (pl_async_macho_t *image, pl_vm_address_t address, plcrash_async_mobject_t *outMobj) {
    pl_vm_address_t cursor = address;
    
    char c;
    do {
        plcrash_error_t err = plcrash_async_read_addr(image->task, cursor, &c, 1);
        if (err != PLCRASH_ESUCCESS) {
            PLCF_DEBUG("in read_string(%p, 0x%llx, %p), plcrash_async_read_addr at 0x%llx failure %d ", image, (long long)address, outMobj, (long long)cursor, err);
            return err;
        }
        cursor++;
    } while(c != 0);
    
    pl_vm_size_t length = cursor - address - 1;
    return plcrash_async_mobject_init(outMobj, image->task, address, length);
}

plcrash_error_t pl_async_objc_parse_from_module_info (pl_async_macho_t *image, pl_async_objc_found_method_cb callback, void *ctx) {
    plcrash_error_t err = PLCRASH_EUNKNOWN;
    
    bool moduleMobjInitialized = false;
    plcrash_async_mobject_t moduleMobj;
    err = pl_async_macho_map_section(image, kObjCSegmentName, kObjCModuleInfoSectionName, &moduleMobj);
    if (err != PLCRASH_ESUCCESS) {
        PLCF_DEBUG("pl_async_macho_map_section(%p, %s, %s, %p) failure %d", image, kObjCSegmentName, kObjCModuleInfoSectionName, &moduleMobj, err);
        goto cleanup;
    }
    
    moduleMobjInitialized = true;
    
    struct pl_objc1_module *moduleData = plcrash_async_mobject_pointer(&moduleMobj, moduleMobj.address, sizeof(*moduleData));
    if (moduleData == NULL) {
        PLCF_DEBUG("Failed to obtain pointer from %s memory object", kObjCModuleInfoSectionName);
        err = PLCRASH_ENOTFOUND;
        goto cleanup;
    }
    
    bool classNameMobjInitialized = false;
    plcrash_async_mobject_t classNameMobj;
    
    for (unsigned moduleIndex = 0; moduleIndex < moduleMobj.length / sizeof(*moduleData); moduleIndex++) {
        pl_vm_address_t symtabPtr = image->swap32(moduleData[moduleIndex].symtab);
        if (symtabPtr == 0)
            continue;
        
        struct pl_objc1_symtab symtab;
        err = plcrash_async_read_addr(image->task, symtabPtr, &symtab, sizeof(symtab));
        if (err != PLCRASH_ESUCCESS) {
            PLCF_DEBUG("plcrash_async_read_addr at 0x%llx error %d", (long long)symtabPtr, err);
            goto cleanup;
        }
        
        uint16_t classCount = image->swap16(symtab.cls_def_count);
        for (unsigned i = 0; i < classCount; i++) {
            uint32_t classPtr;
            pl_vm_address_t cursor = symtabPtr + sizeof(symtab) + i * sizeof(classPtr);
            err = plcrash_async_read_addr(image->task, cursor, &classPtr, sizeof(classPtr));
            if (err != PLCRASH_ESUCCESS) {
                PLCF_DEBUG("plcrash_async_read_addr at 0x%llx error %d", (long long)cursor, err);
                goto cleanup;
            }
            classPtr = image->swap32(classPtr);
            
            struct pl_objc1_class class;
            err = plcrash_async_read_addr(image->task, classPtr, &class, sizeof(class));
            if (err != PLCRASH_ESUCCESS) {
                PLCF_DEBUG("plcrash_async_read_addr at 0x%llx error %d", (long long)classPtr, err);
                goto cleanup;
            }
            
            if (classNameMobjInitialized) {
                plcrash_async_mobject_free(&classNameMobj);
                classNameMobjInitialized = false;
            }
            pl_vm_address_t namePtr = image->swap32(class.name);
            err = read_string(image, namePtr, &classNameMobj);
            if (err != PLCRASH_ESUCCESS) {
                PLCF_DEBUG("read_string at 0x%llx error %d", (long long)namePtr, err);
                goto cleanup;
            }
            classNameMobjInitialized = true;
            
            const char *className = plcrash_async_mobject_pointer(&classNameMobj, classNameMobj.address, classNameMobj.length);
            if (className == NULL) {
                PLCF_DEBUG("Failed to get pointer to class name data");
                goto cleanup;
            }
            
            pl_vm_address_t methodListPtr = image->swap32(class.methods);
            struct pl_objc1_method_list methodList;
            err = plcrash_async_read_addr(image->task, methodListPtr, &methodList, sizeof(methodList));
            if (err != PLCRASH_ESUCCESS) {
                PLCF_DEBUG("plcrash_async_read_addr at 0x%llx error %d", (long long)methodListPtr, err);
                goto cleanup;
            }
            
            uint32_t count = image->swap32(methodList.count);
            for (uint32_t i = 0; i < count; i++) {
                struct pl_objc1_method method;
                pl_vm_address_t methodPtr = methodListPtr + sizeof(methodList) + i * sizeof(method);
                err = plcrash_async_read_addr(image->task, methodPtr, &method, sizeof(method));
                if (err != PLCRASH_ESUCCESS) {
                    PLCF_DEBUG("plcrash_async_read_addr at 0x%llx error %d", (long long)methodPtr, err);
                    goto cleanup;
                }
                
                pl_vm_address_t methodNamePtr = image->swap32(method.name);
                plcrash_async_mobject_t methodNameMobj;
                err = read_string(image, methodNamePtr, &methodNameMobj);
                if (err != PLCRASH_ESUCCESS) {
                    PLCF_DEBUG("read_string at 0x%llx error %d", (long long)methodNamePtr, err);
                    goto cleanup;
                }
                
                const char *methodName = plcrash_async_mobject_pointer(&methodNameMobj, methodNameMobj.address, methodNameMobj.length);
                if (methodName == NULL) {
                    PLCF_DEBUG("Failed to get method name pointer");
                    plcrash_async_mobject_free(&methodNameMobj);
                    goto cleanup;
                }
                
                pl_vm_address_t imp = image->swap32(method.imp);
                
                callback(className, classNameMobj.length, methodName, methodNameMobj.length, imp, ctx);
                
                plcrash_async_mobject_free(&methodNameMobj);
            }
        }
    }
    
cleanup:
    if (moduleMobjInitialized)
        plcrash_async_mobject_free(&moduleMobj);
    if (classNameMobjInitialized)
        plcrash_async_mobject_free(&classNameMobj);
    
    return err;
}

plcrash_error_t pl_async_objc_parse_from_data_section (pl_async_macho_t *image, pl_async_objc_found_method_cb callback, void *ctx) {
    plcrash_error_t err = PLCRASH_EUNKNOWN;
    
    bool classMobjInitialized = false;
    plcrash_async_mobject_t classMobj;
    err = pl_async_macho_map_section(image, kDataSegmentName, kClassListSectionName, &classMobj);
    if (err != PLCRASH_ESUCCESS) {
        PLCF_DEBUG("pl_async_macho_map_section(%p, %s, %s, %p) failure %d", image, kDataSegmentName, kClassListSectionName, &classMobj, err);
        goto cleanup;
    }
    
    classMobjInitialized = true;
    
    plcrash_async_mobject_t classNameMobj;
    bool classNameMobjInitialized = false;
    
    void *classPtrs = plcrash_async_mobject_pointer(&classMobj, classMobj.address, classMobj.length);
    uint32_t *classPtrs_32 = classPtrs;
    uint64_t *classPtrs_64 = classPtrs;
    unsigned classCount = classMobj.length / (image->m64 ? sizeof(*classPtrs_64) : sizeof(*classPtrs_32));
    for(unsigned i = 0; i < classCount; i++) {
        pl_vm_address_t ptr = (image->m64
                               ? image->swap64(classPtrs_64[i])
                               : image->swap32(classPtrs_32[i]));
        
        struct pl_objc2_class_32 class_32;
        struct pl_objc2_class_64 class_64;
        if (image->m64)
            err = plcrash_async_read_addr(image->task, ptr, &class_64, sizeof(class_64));
        else
            err = plcrash_async_read_addr(image->task, ptr, &class_32, sizeof(class_32));
        
        if (err != PLCRASH_ESUCCESS) {
            PLCF_DEBUG("plcrash_async_read_addr at 0x%llx error %d", (long long)ptr, err);
            goto cleanup;
        }
        
        pl_vm_address_t dataPtr = (image->m64
                                   ? image->swap64(class_64.data_rw)
                                   : image->swap32(class_32.data_rw));
        dataPtr &= ~(pl_vm_address_t)3;
        
        struct pl_objc2_class_data_rw_32 classDataRW_32;
        struct pl_objc2_class_data_rw_64 classDataRW_64;
        if (image->m64)
            err = plcrash_async_read_addr(image->task, dataPtr, &classDataRW_64, sizeof(classDataRW_64));
        else
            err = plcrash_async_read_addr(image->task, dataPtr, &classDataRW_32, sizeof(classDataRW_32));
        
        if (err != PLCRASH_ESUCCESS) {
            PLCF_DEBUG("plcrash_async_read_addr at 0x%llx error %d", (long long)dataPtr, err);
            goto cleanup;
        }
        
        pl_vm_address_t dataROPtr = (image->m64
                                     ? image->swap64(classDataRW_64.data_ro)
                                     : image->swap32(classDataRW_32.data_ro));
        
        struct pl_objc2_class_data_ro_32 classDataRO_32;
        struct pl_objc2_class_data_ro_64 classDataRO_64;
        if (image->m64)
            err = plcrash_async_read_addr(image->task, dataROPtr, &classDataRO_64, sizeof(classDataRO_64));
        else
            err = plcrash_async_read_addr(image->task, dataROPtr, &classDataRO_32, sizeof(classDataRO_32));
        
        if (err != PLCRASH_ESUCCESS) {
            PLCF_DEBUG("plcrash_async_read_addr at 0x%llx error %d", (long long)dataROPtr, err);
            goto cleanup;
        }
        
        if (classNameMobjInitialized) {
            plcrash_async_mobject_free(&classNameMobj);
            classNameMobjInitialized = false;
        }
        
        pl_vm_address_t classNamePtr = (image->m64
                                        ? image->swap64(classDataRO_64.name)
                                        : image->swap32(classDataRO_32.name));
        err = read_string(image, classNamePtr, &classNameMobj);
        if (err != PLCRASH_ESUCCESS) {
            PLCF_DEBUG("read_string at 0x%llx error %d", (long long)classNamePtr, err);
            goto cleanup;
        }
        classNameMobjInitialized = true;
        
        const char *className = plcrash_async_mobject_pointer(&classNameMobj, classNameMobj.address, classNameMobj.length);
        if (className == NULL) {
            PLCF_DEBUG("Failed to obtain pointer from class name memory object with address 0x%llx length %llu", (long long)classNameMobj.address, (unsigned long long)classNameMobj.length);
            err = PLCRASH_EACCESS;
            goto cleanup;
        }
        
        pl_vm_address_t methodsPtr = (image->m64
                                      ? image->swap64(classDataRO_64.baseMethods)
                                      : image->swap32(classDataRO_32.baseMethods));
        
        struct pl_objc2_list_header header;
        err = plcrash_async_read_addr(image->task, methodsPtr, &header, sizeof(header));
        if (err != PLCRASH_ESUCCESS) {
            PLCF_DEBUG("plcrash_async_read_addr at 0x%llx error %d", (long long)methodsPtr, err);
            goto cleanup;
        }
        
        uint32_t entsize = image->swap32(header.entsize) & ~(uint32_t)3;
        uint32_t count = image->swap32(header.count);
        
        pl_vm_address_t cursor = methodsPtr + sizeof(header);
        
        for (uint32_t i = 0; i < count; i++) {
            struct pl_objc2_method_32 method_32;
            struct pl_objc2_method_64 method_64;
            if (image->m64)
                err = plcrash_async_read_addr(image->task, cursor, &method_64, sizeof(method_64));
            else
                err = plcrash_async_read_addr(image->task, cursor, &method_32, sizeof(method_32));
            if (err != PLCRASH_ESUCCESS) {
                PLCF_DEBUG("plcrash_async_read_addr at 0x%llx error %d", (long long)cursor, err);
                goto cleanup;
            }
            
            pl_vm_address_t methodNamePtr = (image->m64
                                             ? image->swap64(method_64.name)
                                             : image->swap32(method_32.name));
            
            plcrash_async_mobject_t methodNameMobj;
            err = read_string(image, methodNamePtr, &methodNameMobj);
            if (err != PLCRASH_ESUCCESS) {
                PLCF_DEBUG("read_string at 0x%llx error %d", (long long)methodNamePtr, err);
                goto cleanup;
            }
            
            const char *methodName = plcrash_async_mobject_pointer(&methodNameMobj, methodNameMobj.address, methodNameMobj.length);
            if (methodName == NULL) {
                PLCF_DEBUG("Failed to obtain pointer from method name memory object with address 0x%llx length %llu", (long long)methodNameMobj.address, (unsigned long long)methodNameMobj.length);
                plcrash_async_mobject_free(&methodNameMobj);
                err = PLCRASH_EACCESS;
                goto cleanup;
            }
            
            pl_vm_address_t imp = (image->m64
                                   ? image->swap64(method_64.imp)
                                   : image->swap32(method_32.imp));
            
            callback(className, classNameMobj.length, methodName, methodNameMobj.length, imp, ctx);
            
            plcrash_async_mobject_free(&methodNameMobj);
            
            cursor += entsize;
        }
    }
    
cleanup:
    if (classMobjInitialized)
        plcrash_async_mobject_free(&classMobj);
    if (classNameMobjInitialized)
        plcrash_async_mobject_free(&classNameMobj);
    
    return err;
}

plcrash_error_t pl_async_objc_parse (pl_async_macho_t *image, pl_async_objc_found_method_cb callback, void *ctx) {
    plcrash_error_t err;
    
    err = pl_async_objc_parse_from_module_info(image, callback, ctx);
    if (err == PLCRASH_ENOTFOUND)
        err = pl_async_objc_parse_from_data_section(image, callback, ctx);
    
    return err;
}

struct pl_async_objc_find_method_search_context {
    pl_vm_address_t searchIMP;
    pl_vm_address_t bestIMP;
};

struct pl_async_objc_find_method_call_context {
    pl_vm_address_t searchIMP;
    pl_async_objc_found_method_cb outerCallback;
    void *outerCallbackCtx;
};

static void pl_async_objc_find_method_search_callback (const char *className, pl_vm_size_t classNameLength, const char *methodName, pl_vm_size_t methodNameLength, pl_vm_address_t imp, void *ctx) {
    struct pl_async_objc_find_method_search_context *ctxStruct = ctx;
    
    if (imp >= ctxStruct->bestIMP && imp <= ctxStruct->searchIMP) {
        ctxStruct->bestIMP = imp;
    }
}

static void pl_async_objc_find_method_call_callback (const char *className, pl_vm_size_t classNameLength, const char *methodName, pl_vm_size_t methodNameLength, pl_vm_address_t imp, void *ctx) {
    struct pl_async_objc_find_method_call_context *ctxStruct = ctx;
    
    if (imp == ctxStruct->searchIMP) {
        ctxStruct->outerCallback(className, classNameLength, methodName, methodNameLength, imp, ctxStruct->outerCallbackCtx);
    }
}

plcrash_error_t pl_async_objc_find_method (pl_async_macho_t *image, pl_vm_address_t imp, pl_async_objc_found_method_cb callback, void *ctx) {
    struct pl_async_objc_find_method_search_context searchCtx = {
        .searchIMP = imp
    };
    
    plcrash_error_t err = pl_async_objc_parse(image, pl_async_objc_find_method_search_callback, &searchCtx);
    if (err != PLCRASH_ESUCCESS) {
        PLCF_DEBUG("pl_async_objc_parse(%p, 0x%llx, %p, %p) failure %d", image, (long long)imp, callback, ctx, err);
        return err;
    }
    
    if (searchCtx.bestIMP == 0)
        return PLCRASH_ENOTFOUND;
    
    struct pl_async_objc_find_method_call_context callCtx = {
        .searchIMP = searchCtx.bestIMP,
        .outerCallback = callback,
        .outerCallbackCtx = ctx
    };
    
    return pl_async_objc_parse(image, pl_async_objc_find_method_call_callback, &callCtx);
}
