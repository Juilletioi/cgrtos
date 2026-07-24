/**
 * @file string.c
 * @brief 最小 C 字符串与内存例程
 * @author Cong Zhou / Juilletioi
 * @version 5.0.0
 * @date 2026-07-22
 * @copyright CG-RTOS
 */

#include <stdint.h>
#include <stddef.h>

/**
 * @brief 将内存块填充为指定字节
 * @details 逐字节写入 (unsigned char)c，共 n 字节。纯内存操作，无系统调用，不阻塞、不切换。
 * @param[out] s 目标缓冲区
 * @param[in]  c 填充字节
 * @param[in]  n 字节数
 * @return s 原指针
 * @retval s 填充完成后的缓冲区首地址
 * @note 行为等价于标准库 memset
 * @warning s 为 NULL 且 n>0 时行为未定义
 * @attention ✅ ISR；❌ block/switch
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
 * @brief 复制内存块（源与目标不可重叠）
 * @details 从前向后逐字节复制 n 字节。纯内存操作，不阻塞、不切换。
 * @param[out] d 目标缓冲区
 * @param[in]  s 源缓冲区
 * @param[in]  n 字节数
 * @return d 原指针
 * @retval d 复制完成后的目标首地址
 * @note 重叠区域请使用 memmove
 * @warning 源与目标重叠时结果未定义
 * @attention ✅ ISR；❌ block/switch
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
 * @brief 可重叠的内存移动
 * @details 根据 d 与 s 相对位置选择前向或后向复制，保证重叠安全。纯内存操作，不阻塞、不切换。
 * @param[out] d 目标缓冲区
 * @param[in]  s 源缓冲区
 * @param[in]  n 字节数
 * @return d 原指针
 * @retval d 移动完成后的目标首地址
 * @note d==s 或 n==0 时直接返回，不做复制
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
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
 * @brief 比较两块内存
 * @details 逐字节比较至多 n 字节，遇首个不等字节即返回差值。纯内存操作，不阻塞、不切换。
 * @param[in] s1 缓冲区 1
 * @param[in] s2 缓冲区 2
 * @param[in] n  比较字节数
 * @return 首个不等字节之差 (s1[i]-s2[i])；全部相等返回 0
 * @retval 0     两块内存前 n 字节相同
 * @retval !=0   首个不等字节的 signed 差值
 * @note 按 unsigned char 解释字节
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
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
 * @brief 计算以 NUL 结尾的字符串长度
 * @details 从索引 0 起扫描直至 '\0'，返回字符数（不含终止符）。纯内存操作，不阻塞、不切换。
 * @param[in] s 字符串
 * @return 长度（不含 '\0'）
 * @retval >=0 字符数
 * @note s 须以 NUL 结尾
 * @warning s 为 NULL 时行为未定义
 * @attention ✅ ISR；❌ block/switch
 */
size_t strlen(const char* s){
    size_t l=0;
    /* 1. 逐字符扫描直至 NUL */
    while(s[l])l++;
    /* 2. 返回长度 */
    return l;
}

/**
 * @brief 比较至多 n 个字符
 * @details 逐字符比较 s1 与 s2，至多 n 次；s1 遇 NUL 提前结束视为相等。不阻塞、不切换。
 * @param[in] s1 字符串 1
 * @param[in] s2 字符串 2
 * @param[in] n  最大比较长度
 * @return 首个不等字符之差；相等或均遇 '\0' 返回 0
 * @retval 0     前 n 字符（或至 NUL）相同
 * @retval !=0   首个不等字符的 unsigned char 差值
 * @note 按 unsigned char 比较
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
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
 * @brief 复制至多 n 个字符并补 NUL
 * @details 从 s 复制字符到 d，最多 n 个，遇 s[i]=='\0' 停止；剩余位置填充 '\0'。不阻塞、不切换。
 * @param[out] d 目标缓冲区
 * @param[in]  s 源字符串
 * @param[in]  n 目标容量（含终止符空间）
 * @return d 原指针
 * @retval d 复制完成后的目标首地址
 * @note 若源长度 >= n，结果可能不以 NUL 结尾（与 POSIX 一致）
 * @warning 目标缓冲区须至少 n 字节
 * @attention ✅ ISR；❌ block/switch
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
 * @brief 全串比较
 * @details 等价于 strncmp(s1,s2,SIZE_MAX) 语义至双 NUL。
 * @param[in] s1 串 1
 * @param[in] s2 串 2
 * @return 首个不等字符之差；相等为 0
 * @retval 0 相等
 * @retval !=0 差值
 * @note 无
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
int strcmp(const char* s1,const char* s2){
    while(*s1&&*s1==*s2){s1++;s2++;}
    return(unsigned char)*s1-(unsigned char)*s2;
}

/**
 * @brief 从右查找字符
 * @details 自左向右扫描并记录最后一次命中位置；含查找 '\\0' 时返回指向终止符的指针。
 * @param[in] s 串
 * @param[in] c 目标字符（转 unsigned char）
 * @return 最后一次出现的指针；未找到 NULL
 * @retval 非 NULL 命中
 * @retval NULL 未命中
 * @note 含查找 '\\0' 时返回指向终止符的指针
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
char* strrchr(const char* s,int c){
    const char* last=0;
    unsigned char ch=(unsigned char)c;
    do{
        if((unsigned char)*s==ch)last=s;
    }while(*s++);
    return(char*)last;
}

/**
 * @brief 有界追加
 * @details 向 d 末尾追加至多 n 个来自 s 的字符，并保证 NUL（若空间允许）。
 * @param[out] d 目标（须已 NUL 结尾且有足够空间）
 * @param[in]  s 源
 * @param[in]  n 最多追加字符数（不含强制 NUL 的约定依 POSIX）
 * @return d
 * @retval d 目标
 * @note 若 d 长度 + n 空间不足，调用方须保证缓冲
 * @warning 无
 * @attention ✅ ISR；❌ block/switch
 */
char* strncat(char* d,const char* s,size_t n){
    char* p=d;
    while(*p)p++;
    while(n&&*s){*p++=*s++;n--;}
    *p=0;
    return d;
}

/**
 * @brief 受限格式化输出（子集：%s %d %i %u %x %lu %c %%）
 * @details 解析 fmt 并将结果写入 buf，保留至少 1 字节给 NUL。纯栈上格式化，不阻塞、不切换（不含浮点/宽度/精度）。
 * @param[out] buf 输出缓冲区
 * @param[in]  n   缓冲区容量（含终止符）
 * @param[in]  fmt 格式串
 * @param[in]  ... 可变参数
 * @return 写入字符数（不含 NUL）
 * @retval >=0 实际写入字符数
 * @note 不支持 %p、%lld 等扩展格式
 * @warning buf 为 NULL 或 n==0 时不写入；超长输出会被截断
 * @attention ✅ ISR；❌ block/switch
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
