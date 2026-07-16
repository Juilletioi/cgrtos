/**
 * @file string.c
 * @brief 最小 C 字符串与内存例程
 * @details 提供 memset/memcpy/memcmp/strlen/strncmp/strncpy 及
 *          cgrtos_snprintf，供无 libc 环境下内核与驱动使用。
 */
#include <stdint.h>
#include <stddef.h>

/**
 * @brief 将内存块填充为指定字节。
 *
 * @param s 目标缓冲区。
 * @param c 填充字节。
 * @param n 字节数。
 * @return s 原指针。
 *
 * @details
 * 1. 将 s 转为 unsigned char 指针。
 * 2. 循环 n 次，逐字节写入 (unsigned char)c。
 * 3. 返回原始指针 s。
 */
void* memset(void* s,int c,size_t n){
    /* 1. 转为 unsigned char 指针 */
    unsigned char* p=(unsigned char*)s;
    /* 2. 逐字节填充 */
    for(size_t i=0;i<n;i++)p[i]=(unsigned char)c;
    /* 3. 返回原始指针 */
    return s;
}

/**
 * @brief 复制内存块（源与目标不可重叠）。
 *
 * @param d 目标缓冲区。
 * @param s 源缓冲区。
 * @param n 字节数。
 * @return d 原指针。
 *
 * @details
 * 1. 将 d、s 转为 unsigned char 指针。
 * 2. 循环 n 次，逐字节从源复制到目标。
 * 3. 返回目标指针 d。
 */
void* memcpy(void* d,const void* s,size_t n){
    /* 1. 转为 unsigned char 指针 */
    unsigned char* dd=(unsigned char*)d;
    const unsigned char* ss=(const unsigned char*)s;
    /* 2. 逐字节复制 */
    for(size_t i=0;i<n;i++)dd[i]=ss[i];
    /* 3. 返回目标指针 */
    return d;
}

/**
 * @brief 可重叠的内存移动。
 *
 * @param d 目标缓冲区。
 * @param s 源缓冲区。
 * @param n 字节数。
 * @return d 原指针。
 *
 * @details
 * 1. 若 d==s 或 n==0，无需移动，直接返回 d。
 * 2. 若 d < s（目标在前），从前向后逐字节复制，避免覆盖未读源数据。
 * 3. 若 d > s（目标在后），从后向前逐字节复制，避免覆盖未读源数据。
 * 4. 返回目标指针 d。
 */
void* memmove(void* d,const void* s,size_t n){
    unsigned char* dd=(unsigned char*)d;
    const unsigned char* ss=(const unsigned char*)s;
    /* 1. 无需移动则直接返回 */
    if(dd==ss||n==0)return d;
    /* 2. 目标在前：从前向后复制 */
    if(dd<ss){for(size_t i=0;i<n;i++)dd[i]=ss[i];}
    /* 3. 目标在后：从后向前复制 */
    else{for(size_t i=n;i>0;i--)dd[i-1]=ss[i-1];}
    /* 4. 返回目标指针 */
    return d;
}

/**
 * @brief 比较两块内存。
 *
 * @param s1 缓冲区 1。
 * @param s2 缓冲区 2。
 * @param n  比较字节数。
 * @return 首个不等字节之差 (s1[i]-s2[i])，相等返回 0。
 *
 * @details
 * 1. 将 s1、s2 转为 unsigned char 指针。
 * 2. 逐字节比较，遇不等立即返回差值。
 * 3. 全部相等则返回 0。
 */
int memcmp(const void* s1,const void* s2,size_t n){
    /* 1. 转为 unsigned char 指针 */
    const unsigned char* p1=(const unsigned char*)s1;
    const unsigned char* p2=(const unsigned char*)s2;
    /* 2. 逐字节比较，不等立即返回 */
    for(size_t i=0;i<n;i++){if(p1[i]!=p2[i])return p1[i]-p2[i];}
    /* 3. 全部相等 */
    return 0;
}

/**
 * @brief 计算以 NUL 结尾的字符串长度。
 *
 * @param s 字符串。
 * @return 长度（不含终止符 '\0'）。
 *
 * @details
 * 1. 从索引 0 起逐字符检查，直至遇到 '\0'。
 * 2. 返回已扫描字符数。
 */
size_t strlen(const char* s){
    size_t l=0;
    /* 1. 逐字符扫描直至 NUL */
    while(s[l])l++;
    /* 2. 返回长度 */
    return l;
}

/**
 * @brief 比较至多 n 个字符。
 *
 * @param s1 字符串 1。
 * @param s2 字符串 2。
 * @param n  最大比较长度。
 * @return 首个不等字符之差，相等或均遇 '\0' 返回 0。
 *
 * @details
 * 1. 循环至多 n 次，逐字符比较 s1[i] 与 s2[i]。
 * 2. 遇不等返回 (unsigned char) 差值。
 * 3. 若 s1[i]=='\0' 提前结束，视为相等。
 * 4. 循环结束返回 0。
 */
int strncmp(const char* s1,const char* s2,size_t n){
    /* 1. 至多 n 次逐字符比较 */
    for(size_t i=0;i<n;i++){
        /* 2. 不等则返回差值 */
        if(s1[i]!=s2[i])return(unsigned char)s1[i]-(unsigned char)s2[i];
        /* 3. s1 遇 NUL 提前结束视为相等 */
        if(!s1[i])break;
    }
    /* 4. 全部相等 */
    return 0;
}

/**
 * @brief 复制至多 n 个字符并补 NUL。
 *
 * @param d 目标缓冲区。
 * @param s 源字符串。
 * @param n 目标容量。
 * @return d 原指针。
 *
 * @details
 * 1. 从 s 复制字符到 d，最多 n 个，遇 s[i]=='\0' 停止。
 * 2. 若复制不足 n 个，剩余位置填充 '\0'。
 * 3. 返回目标指针 d。
 */
char* strncpy(char* d,const char* s,size_t n){
    size_t i;
    /* 1. 复制至多 n 个字符，遇 NUL 停止 */
    for(i=0;i<n&&s[i];i++)d[i]=s[i];
    /* 2. 剩余位置补 NUL */
    for(;i<n;i++)d[i]=0;
    /* 3. 返回目标指针 */
    return d;
}

/**
 * @brief 受限格式化输出（子集：%s %d %i %u %x %lu %c %%）。
 *
 * @param buf 输出缓冲区。
 * @param n   缓冲区容量（含终止符）。
 * @param fmt 格式串。
 * @param ... 可变参数。
 * @return 写入字符数（不含 NUL）。
 *
 * @details
 * 1. 初始化可变参数列表 ap。
 * 2. 逐字符扫描 fmt，保留至少 1 字节给 NUL（rem > 1）：
 *    a. 普通字符直接写入 buf；
 *    b. '%' 后解析格式符，从 ap 取参并格式化写入。
 * 3. 支持 %s %d %i %u %x %lu %c %%。
 * 4. 在 buf 末尾写入 '\0'（若 n > 0）。
 * 5. 返回已写入字符数 r。
 */
int cgrtos_snprintf(char* buf,size_t n,const char* fmt,...){
    /* 1. 初始化可变参数列表 */
    __builtin_va_list ap; __builtin_va_start(ap,fmt);
    int r=0; char* p=buf; size_t rem=n;
    /* 2. 逐字符扫描 fmt，保留 1 字节给 NUL */
    for(const char* f=fmt;*f&&rem>1;f++){
        if(*f!='%'){*p++=*f;r++;rem--;continue;} f++;
        if(*f=='s'){const char* s=__builtin_va_arg(ap,const char*);if(!s)s="(null)";while(*s&&rem>1){*p++=*s++;r++;rem--;}}
        else if(*f=='d'||*f=='i'){int v=__builtin_va_arg(ap,int);if(v<0){if(rem>1){*p++='-';r++;rem--;}v=-v;}char t[12];int ti=0;if(!v)t[ti++]='0';while(v&&ti<12){t[ti++]='0'+v%10;v/=10;}while(ti>0&&rem>1){ti--;*p++=t[ti];r++;rem--;}}
        else if(*f=='u'){unsigned v=__builtin_va_arg(ap,unsigned);char t[12];int ti=0;if(!v)t[ti++]='0';while(v&&ti<12){t[ti++]='0'+v%10;v/=10;}while(ti>0&&rem>1){ti--;*p++=t[ti];r++;rem--;}}
        else if(*f=='x'){unsigned v=__builtin_va_arg(ap,unsigned);char t[16];int ti=0;while(v&&ti<16){int d=v&0xF;t[ti++]=(d<10)?'0'+d:'a'+d-10;v>>=4;}if(!ti)t[ti++]='0';while(ti>0&&rem>1){ti--;*p++=t[ti];r++;rem--;}}
        else if(*f=='l'){f++;if(*f=='u'){unsigned long v=__builtin_va_arg(ap,unsigned long);char t[20];int ti=0;if(!v)t[ti++]='0';while(v&&ti<20){t[ti++]='0'+v%10;v/=10;}while(ti>0&&rem>1){ti--;*p++=t[ti];r++;rem--;}}}
        else if(*f=='c'){if(rem>1){*p++=(char)__builtin_va_arg(ap,int);r++;rem--;}}
        else if(*f=='%'){if(rem>1){*p++='%';r++;rem--;}}
    }
    /* 4. 写入 NUL 终止符 */
    __builtin_va_end(ap); if(buf&&n>0)*p=0;
    /* 5. 返回已写入字符数 */
    return r;
}
