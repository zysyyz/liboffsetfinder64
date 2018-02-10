//
//  offsetfinder64.cpp
//  offsetfinder64
//
//  Created by tihmstar on 10.01.18.
//  Copyright © 2018 tihmstar. All rights reserved.
//

#include "offsetfinder64.hpp"


extern "C"{
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include "img4.h"
#include "lzssdec.h"
}

#define info(a ...) ({printf(a),printf("\n");})
#define log(a ...) ({if (dbglog) printf(a),printf("\n");})
#define warning(a ...) ({if (dbglog) printf("[WARNING] "), printf(a),printf("\n");})
#define error(a ...) ({printf("[Error] "),printf(a),printf("\n");})

#define safeFree(ptr) ({if (ptr) free(ptr),ptr=NULL;})

#define reterror(err) throw tihmstar::exception(__LINE__,err)
#define assure(cond) if ((cond) == 0) throw tihmstar::exception(__LINE__, "assure failed")
#define doassure(cond,code) do {if (!(cond)){(code);assure(cond);}} while(0)
#define retassure(cond, err) if ((cond) == 0) throw tihmstar::exception(__LINE__,err)
#define assureclean(cond) do {if (!(cond)){clean();assure(cond);}} while(0)


#ifdef DEBUG
#define OFFSETFINDER64_VERSION_COMMIT_COUNT "debug"
#define OFFSETFINDER64_VERSION_COMMIT_SHA "debug build"
#endif

using namespace std;
using namespace tihmstar;
using namespace patchfinder64;

using segment_t = std::vector<offsetfinder64::text_t>;

#define HAS_BITS(a,b) (((a) & (b)) == (b))
#define _symtab getSymtab()
int decompress_lzss(u_int8_t *dst, u_int8_t *src, u_int32_t srclen);

namespace patchfinder64 {
    class insn;
}


#pragma mark macho external

__attribute__((always_inline)) struct load_command *find_load_command64(struct mach_header_64 *mh, uint32_t lc){
    struct load_command *lcmd = (struct load_command *)(mh + 1);
    for (uint32_t i=0; i<mh->ncmds; i++, lcmd = (struct load_command *)((uint8_t *)lcmd + lcmd->cmdsize)) {
        if (lcmd->cmd == lc)
            return lcmd;
    }
    
    reterror("Failed to find load command "+ to_string(lc));
    return NULL;
}

__attribute__((always_inline)) struct symtab_command *find_symtab_command(struct mach_header_64 *mh){
    return (struct symtab_command *)find_load_command64(mh, LC_SYMTAB);
}

__attribute__((always_inline)) struct dysymtab_command *find_dysymtab_command(struct mach_header_64 *mh){
    return (struct dysymtab_command *)find_load_command64(mh, LC_DYSYMTAB);
}

__attribute__((always_inline)) struct section_64 *find_section(struct segment_command_64 *seg, const char *sectname){
    struct section_64 *sect = (struct section_64 *)(seg + 1);
    for (uint32_t i=0; i<seg->nsects; i++, sect++) {
        if (strcmp(sect->sectname, sectname) == 0)
            return sect;
    }
    reterror("Failed to find section "+ string(sectname));
    return NULL;
}

offsetfinder64::offsetfinder64(const char* filename) : _freeKernel(true),__symtab(NULL){
    struct stat fs = {0};
    int fd = 0;
    char *img4tmp = NULL;
    auto clean =[&]{
        if (fd>0) close(fd);
    };
    assure((fd = open(filename, O_RDONLY)) != -1);
    assureclean(!fstat(fd, &fs));
    assureclean((_kdata = (uint8_t*)malloc( _ksize = fs.st_size)));
    assureclean(read(fd,_kdata,_ksize)==_ksize);
    
    //check if feedfacf, lzss, img4, im4p
    img4tmp = (char*)_kdata;
    if (sequenceHasName(img4tmp, (char*)"IMG4")){
        img4tmp = getElementFromIMG4((char*)_kdata, (char*)"IM4P");
    }
    if (sequenceHasName(img4tmp, (char*)"IM4P")){
        /*extract file from IM4P*/
        char *extractFile = [](char *buf, char **dstBuf)->char*{
            int elems = asn1ElementsInObject(buf);
            if (elems < 4){
                error("not enough elements in SEQUENCE %d\n",elems);
                return NULL;
            }
            
            char *dataTag = asn1ElementAtIndex(buf, 3)+1;
            t_asn1ElemLen dlen = asn1Len(dataTag);
            char *data = dataTag+dlen.sizeBytes;
            
            char *kernel = NULL;
            if ((kernel = tryLZSS(data, (size_t*)&dlen.dataLen))){
                data = kernel;
                printf("lzsscomp detected, uncompressing...\n");
            }
            return kernel;
        }(img4tmp,&extractFile);
        /* done extract file from IM4P*/
        
        free(_kdata);
        _kdata = (uint8_t*)extractFile;
    }
    
    assureclean(*(uint32_t*)_kdata == 0xfeedfacf);
    
    loadSegments(0);
    clean();
}

void offsetfinder64::loadSegments(uint64_t slide){
    printf("getting kernelbase: ");
    _kslide = slide;
    struct mach_header_64 *mh = (struct mach_header_64*)_kdata;
    struct load_command *lcmd = (struct load_command *)(mh + 1);
    for (uint32_t i=0; i<mh->ncmds; i++, lcmd = (struct load_command *)((uint8_t *)lcmd + lcmd->cmdsize)) {
        if (lcmd->cmd == LC_SEGMENT_64){
            struct segment_command_64* seg = (struct segment_command_64*)lcmd;
            _segments.push_back({_kdata+seg->fileoff,seg->filesize, (loc_t)seg->vmaddr, (seg->maxprot & VM_PROT_EXECUTE) !=0});
        }
    }
    
    info("Inited offsetfinder64 %s %s\n",OFFSETFINDER64_VERSION_COMMIT_COUNT, OFFSETFINDER64_VERSION_COMMIT_SHA);
    
}

offsetfinder64::offsetfinder64(void* buf, size_t size, uint64_t slide) : _freeKernel(false),_kdata((uint8_t*)buf),_ksize(size),__symtab(NULL){
    loadSegments(slide);
}



#pragma mark macho offsetfinder
__attribute__((always_inline)) struct symtab_command *offsetfinder64::getSymtab(){
    if (!__symtab)
        __symtab = find_symtab_command((struct mach_header_64 *)_kdata);
    return __symtab;
}

#pragma mark offsetfidner

loc_t offsetfinder64::memmem(const void *little, size_t little_len){
    for (auto seg : _segments) {
        if (loc_t rt = (loc_t)::memmem(seg.map, seg.size, little, little_len)) {
            return rt-seg.map+seg.base+_kslide;
        }
    }
    return 0;
}


loc_t offsetfinder64::find_sym(const char *sym){
    uint8_t *psymtab = _kdata + _symtab->symoff;
    uint8_t *pstrtab = _kdata + _symtab->stroff;

    struct nlist_64 *entry = (struct nlist_64 *)psymtab;
    for (uint32_t i = 0; i < _symtab->nsyms; i++, entry++)
        if (!strcmp(sym, (char*)(pstrtab + entry->n_un.n_strx)))
            return (loc_t)entry->n_value;

    reterror("Failed to find symbol "+string(sym));
    return 0;
}

#pragma mark patchfinder64
namespace tihmstar{
    namespace patchfinder64{
        
        class insn{
            std::pair <loc_t,int> _p;
            std::vector<offsetfinder64::text_t> _segments;
            offset_t _kslide;
            bool _textOnly;
        public:
            insn(segment_t segments, offset_t kslide, loc_t p = 0, bool textOnly = 1) : _segments(segments), _kslide(kslide), _textOnly(textOnly){
                std::sort(_segments.begin(),_segments.end(),[ ]( const offsetfinder64::text_t& lhs, const offsetfinder64::text_t& rhs){
                    return lhs.base < rhs.base;
                });
                if (_textOnly) {
                    _segments.erase(std::remove_if(_segments.begin(), _segments.end(), [](const offsetfinder64::text_t obj){
                        return !obj.isExec;
                    }));
                }
                if (p == 0) {
                    p = _segments.at(0).base;
                }
                for (int i=0; i<_segments.size(); i++){
                    auto seg = _segments[i];
                    if ((loc_t)seg.base <= p && p < (loc_t)seg.base+seg.size){
                        _p = {p,i};
                        return;
                    }
                }
                reterror("initializing insn with out of range location");
            }
            
            insn(const insn &cpy, loc_t p=0){
                _segments = cpy._segments;
                _kslide = cpy._kslide;
                _textOnly = cpy._textOnly;
                if (p==0) {
                    _p = cpy._p;
                }else{
                    for (int i=0; i<_segments.size(); i++){
                        auto seg = _segments[i];
                        if ((loc_t)seg.base <= p && p < (loc_t)seg.base+seg.size){
                            _p = {p,i};
                            return;
                        }
                    }
                    reterror("initializing insn with out of range location");
                }
            }
            
            insn &operator++(){
                _p.first+=4;
                if (_p.first >=_segments[_p.second].base+_segments[_p.second].size){
                    if (_p.second+1 < _segments.size()) {
                        _p.first = _segments[++_p.second].base;
                    }else{
                        _p.first-=4;
                        throw out_of_range("overflow");
                    }
                }
                return *this;
            }
            insn &operator--(){
                _p.first-=4;
                if (_p.first < _segments[_p.second].base){
                    if (_p.second-1 >0) {
                        --_p.second;
                        _p.first = _segments[_p.second].base+_segments[_p.second].size;
                    }else{
                        _p.first+=4;
                        throw out_of_range("underflow");
                    }
                }
                return *this;
            }
            insn operator+(int i){
                insn cpy(*this);
                if (i>0) {
                    while (i--)
                        ++cpy;
                }else{
                    while (i++)
                        --cpy;
                }
                return cpy;
            }
            insn operator-(int i){
                return this->operator+(-i);
            }
            
        public: //helpers
            int64_t signExtend64(uint64_t v, int vSize){
                uint64_t e = (v & 1 << (vSize-1))>>(vSize-1);
                for (int i=vSize; i<64; i++)
                    v |= e << i;
                return v;
            }
            uint64_t pc(){
                return (uint64_t)_p.first + (uint64_t)_kslide;
            }
            uint32_t value(){
                return (*(uint32_t*)(loc_t)(*this));
            }
            
        public: //static type determinition
            static uint64_t deref(segment_t segments, offset_t kslide, loc_t p){
                return *(uint64_t*)(loc_t)insn(segments, kslide, p,0);
            }
            static bool is_adrp(uint32_t i){
                return ((i>>24) % (1<<5)) == 0b10000 && (i>>31);
            }
            static bool is_add(uint32_t i){
                return ((i>>24) % (1<<5)) == 0b10001;
            }
            static bool is_bl(uint32_t i){
                return (i>>26) == 0b100101;
            }
            static bool is_cbz(uint32_t i){
                return ((i>>24) % (1<<7)) == 0b0110100;
            }
            static bool is_ret(uint32_t i){
                return ((0b11111 << 5) | i) == 0b11010110010111110000001111100000;
            }
            static bool is_tbnz(uint32_t i){
                return ((i>>24) % (1<<7)) == 0b0110111;
            }
            static bool is_br(uint32_t i){
                return ((0b11111 << 5) | i) == 0b11010110000111110000001111100000;
            }
            static bool is_ldr(uint32_t i){
                return (((i>>22) | 0b0100000000) == 0b1111100001 && ((i>>10) % 4)) || ((i>>22 | 0b0100000000) == 0b1111100101) || ((i>>23) == 0b00011000);
            }
            static bool is_cbnz(uint32_t i){
                return ((i>>24) % (1<<7)) == 0b0110101;
            }
            
            
        public: //type
            enum type{
                unknown,
                adrp,
                bl,
                cbz,
                ret,
                tbnz,
                add,
                br,
                ldr,
                cbnz
            };
            enum subtype{
                st_general,
                st_register,
                st_immediate,
                st_literal
            };
            enum supertype{
                sut_general,
                sut_branch_imm
            };
            type type(){
                uint32_t val = value();
                if (is_adrp(val))
                    return adrp;
                else if (is_add(val))
                    return add;
                else if (is_bl(val))
                    return bl;
                else if (is_cbz(val))
                    return cbz;
                else if (is_ret(val))
                    return ret;
                else if (is_tbnz(val))
                    return tbnz;
                else if (is_br(val))
                    return br;
                else if (is_ldr(val))
                    return ldr;
                else if (is_cbnz(val))
                    return cbnz;
                
                return unknown;
            }
            subtype subtype(){
                uint32_t i = value();
                if (is_ldr(i)) {
                    if ((((i>>22) | 0b0100000000) == 0b1111100001) && ((i>>10) % 4) == 0b10)
                        return st_register;
                    else if (i>>31)
                        return st_immediate;
                    else
                        return st_literal;
                    
                }
                return st_general;
            }
            supertype supertype(){
                switch (type()) {
                    case bl:
                    case cbz:
                    case cbnz:
                    case tbnz:
                        return sut_branch_imm;
                        
                    default:
                        return sut_general;
                }
            }
            int64_t imm(){
                switch (type()) {
                    case unknown:
                        reterror("can't get imm value of unknown instruction");
                        break;
                    case adrp:
                        return ((pc()>>12)<<12) + signExtend64(((((value() % (1<<24))>>5)<<2) | ((value()>>29) % (1<<2)))<<12,32);
                    case add:
                        return ((value()>>10) % (1<<12)) << (((value()>>22)&1) * 12);
                    case bl:
                        return signExtend64(value() % (1<<26), 25); //untested
                    case cbz:
                    case cbnz:
                    case tbnz:
                        return signExtend64((value() >> 5) % (1<<19), 19); //untested
                    case ldr:
                        if(subtype() != st_immediate){
                            reterror("can't get imm value of ldr that has non immediate subtype");
                            break;
                        }
                        if((value()>>24) % (1<<2)){
                            // Unsigned Offset
                            return ((value()>>10) % (1<<12)) << (value()>>30);
                        }else{
                            // Signed Offset
                            return signExtend64((value()>>12) % 1024, 9); //untested
                        }
                    default:
                        reterror("failed to get imm value");
                        break;
                }
                return 0;
            }
            uint8_t rd(){
                switch (type()) {
                    case unknown:
                        reterror("can't get rd of unknown instruction");
                        break;
                    case adrp:
                    case add:
                        return (value() % (1<<5));
                        
                    default:
                        reterror("failed to get rd");
                        break;
                }
            }
            uint8_t rn(){
                switch (type()) {
                    case unknown:
                        reterror("can't get rn of unknown instruction");
                        break;
                    case add:
                    case ret:
                    case br:
                        return ((value() >>5) % (1<<5));
                        
                    default:
                        reterror("failed to get rn");
                        break;
                }
            }
            uint8_t rt(){
                switch (type()) {
                    case unknown:
                        reterror("can't get rt of unknown instruction");
                        break;
                    case cbz:
                    case cbnz:
                    case tbnz:
                        return (value() % (1<<5));
                        
                    default:
                        reterror("failed to get rt");
                        break;
                }
            }
        public: //cast operators
            operator loc_t(){
                return (loc_t)(_p.first - _segments[_p.second].base + _segments[_p.second].map);
            }
            operator enum type(){
                return type();
            }
        };
        
        loc_t find_literal_ref(segment_t segemts, offset_t kslide, loc_t pos){
            insn adrp(segemts,kslide);
            
            uint8_t rd = 0xff;
            uint64_t imm = 0;
            try {
                while (1){
                    if (adrp.type() == insn::adrp) {
                        rd = adrp.rd();
                        imm = adrp.imm();
                    }else if (adrp.type() == insn::add && rd == adrp.rd()){
                        if (imm + adrp.imm() == (uint64_t)pos)
                            return (loc_t)adrp.pc();
                    }
                    ++adrp;
                }
                
                
            } catch (std::out_of_range &e) {
                return 0;
            }
            return 0;
        }
        loc_t find_rel_branch_source(insn bdst, bool searchUp){
            insn bsrc(bdst);
            
            while (true) {
                if (searchUp)
                    while ((--bsrc).supertype() != insn::sut_branch_imm);
                else
                    while ((++bsrc).supertype() != insn::sut_branch_imm);
                
                if (bsrc.imm()*4 + bsrc.pc() == bdst.pc()) {
                    return (loc_t)bsrc.pc();
                }
            }
            return 0;
        }

    };
};

namespace tihmstar{
    namespace patchfinder64{
        
        
        loc_t jump_stub_call_ptr_loc(insn bl_insn){
            assure(bl_insn == insn::bl);
            insn fdst(bl_insn,(loc_t)(bl_insn.imm()*4+bl_insn.pc()));
            insn ldr((fdst+1));
            retassure((fdst == insn::adrp && ldr == insn::ldr && (fdst+2) == insn::br), "branch destination not jump_stub_call");
            return (loc_t)fdst.imm() + ldr.imm();
        }
        
        bool is_call_to_jump_stub(insn bl_insn){
            try {
                jump_stub_call_ptr_loc(bl_insn);
                return true;
            } catch (tihmstar::exception &e) {
                return false;
            }
        }
        
    }
}



patch offsetfinder64::find_sandbox_patch(){
    loc_t str = memmem("process-exec denied while updating label", sizeof("process-exec denied while updating label")-1);
    retassure(str, "Failed to find str");

    loc_t ref = find_literal_ref(_segments, _kslide, str);
    retassure(ref, "literal ref to str");

    insn bdst(_segments, _kslide, ref);
    for (int i=0; i<4; i++) {
        while (--bdst != insn::bl){
        }
    }
    --bdst;
    
    loc_t cbz = find_rel_branch_source(bdst, true);
    
    constexpr char nop[] = "\x1F\x20\x03\xD5";
    return patch(cbz, nop, sizeof(nop)-1);
}


patch offsetfinder64::find_amfi_substrate_patch(){
    loc_t str = memmem("AMFI: hook..execve() killing pid %u: %s", sizeof("AMFI: hook..execve() killing pid %u: %s")-1);
    retassure(str, "Failed to find str");

    loc_t ref = find_literal_ref(_segments, _kslide, str);
    retassure(ref, "literal ref to str");

    insn funcend(_segments, _kslide, ref);
    while (++funcend != insn::ret);
    
    insn tbnz(funcend);
    while (--tbnz != insn::tbnz);
    
    constexpr char mypatch[] = "\x1F\x20\x03\xD5\x00\x78\x16\x12\x1F\x20\x03\xD5\x00\x00\x80\x52\xE9\x01\x80\x52";
    return {(loc_t)tbnz.pc(),mypatch,sizeof(mypatch)-1};
}

patch offsetfinder64::find_cs_enforcement_disable_amfi(){
    loc_t str = memmem("csflags", sizeof("csflags"));
    retassure(str, "Failed to find str");
    
    loc_t ref = find_literal_ref(_segments, _kslide, str);
    retassure(ref, "literal ref to str");

    insn cbz(_segments, _kslide, ref);
    while (--cbz != insn::cbz);
    
    insn ret(cbz);
    while (++ret != insn::ret);

    int anz = static_cast<int>((ret.pc()-cbz.pc())/4 +1);
    
    constexpr char nop[] = "\x1F\x20\x03\xD5";
    char mypatch[anz*4];
    for (int i=0; i<anz; i++) {
        ((uint32_t*)mypatch)[i] = *(uint32_t*)nop;
    }

    return {(loc_t)cbz.pc(),mypatch,static_cast<size_t>(anz*4)};
}

patch offsetfinder64::find_i_can_has_debugger_patch_off(){
    loc_t str = memmem("Darwin Kernel", sizeof("Darwin Kernel")-1);
    retassure(str, "Failed to find str");
    
    str -=4;
    
    return {str,"\x01",1};
}

patch offsetfinder64::find_amfi_patch_offsets(){
    loc_t str = memmem("int _validateCodeDirectoryHashInDaemon", sizeof("int _validateCodeDirectoryHashInDaemon")-1);
    retassure(str, "Failed to find str");
    
    loc_t ref = find_literal_ref(_segments, _kslide, str);
    retassure(ref, "literal ref to str");

    insn bl_amfi_memcp(_segments, _kslide, ref);

    loc_t jscpl = 0;
    while (1) {
        while (++bl_amfi_memcp != insn::bl);
        
        try {
            jscpl = jump_stub_call_ptr_loc(bl_amfi_memcp);
        } catch (tihmstar::exception &e) {
            continue;
        }
        if (insn::deref(_segments, _kslide, jscpl) == (uint64_t)find_sym("_memcmp"))
            break;
    }
    
    /* find*/
    //movz w0, #0x0
    //ret
    insn ret0(_segments, _kslide, find_sym("_memcmp"));
    for (;; --ret0) {
        if (ret0.value() == *(uint32_t*)"\x00\x00\x80\x52" //movz       w0, #0x0
            && (ret0+1) == insn::ret) {
            break;
        }
    }
    
    uint64_t gadget = ret0.pc();
    return {jscpl,&gadget,sizeof(gadget)};
}

patch offsetfinder64::find_proc_enforce(){
    loc_t str = memmem("Enforce MAC policy on process operations", sizeof("Enforce MAC policy on process operations")-1);
    retassure(str, "Failed to find str");
    
    loc_t valref = memmem(&str, sizeof(str));
    retassure(valref, "Failed to find val ref");
    
    loc_t proc_enforce_ptr = valref - (5 * sizeof(uint64_t));
    
    loc_t proc_enforce_val_loc = (loc_t)insn::deref(_segments, _kslide, proc_enforce_ptr);
    
    uint8_t mypatch = 1;
    return {proc_enforce_val_loc,&mypatch,1};
}

vector<patch> offsetfinder64::find_nosuid_off(){
    loc_t str = memmem("\"mount_common(): mount of %s filesystem failed with %d, but vnode list is not empty.\"", sizeof("\"mount_common(): mount of %s filesystem failed with %d, but vnode list is not empty.\"")-1);
    retassure(str, "Failed to find str");
    
    loc_t ref = find_literal_ref(_segments, _kslide, str);
    retassure(ref, "literal ref to str");

    insn ldr(_segments, _kslide,ref);
    
    while (--ldr != insn::ldr);
    
    loc_t cbnz = find_rel_branch_source(ldr, 1);
    
    insn bl_vfs_context_is64bit(ldr,cbnz);
    while (--bl_vfs_context_is64bit != insn::bl || bl_vfs_context_is64bit.imm()*4+bl_vfs_context_is64bit.pc() != (uint64_t)find_sym("_vfs_context_is64bit"));
        

    
    
    printf("");
    return {};
}




offsetfinder64::~offsetfinder64(){
    if (_freeKernel) safeFree(_kdata);
}










//