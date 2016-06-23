libatbus
========

用于搭建高性能、全异步(a)、树形结构(t)的BUS消息系统的跨平台框架库

> Build & Run Unit Test in |  Linux+OSX(clang+gcc) | Windows+MinGW(vc+gcc) |
> -------------------------|--------|---------|
> Status |  [![Build Status](https://travis-ci.org/atframework/libatbus.svg?branch=master)](https://travis-ci.org/atframework/libatbus) | [![Build status](https://ci.appveyor.com/api/projects/status/v2ufe4xuwbc6gjlf/branch/master?svg=true)](https://ci.appveyor.com/project/owt5008137/libatbus-k408k/branch/master) |
>

Gitter
------
[![Gitter](https://badges.gitter.im/owt5008137/libatbus.svg)](https://gitter.im/owt5008137/libatbus?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge)

依赖
------

+ 支持c++0x或c++11的编译器(为了代码尽量简洁,特别是少做无意义的平台兼容，依赖部分 C11和C++11的功能，所以不支持过低版本的编译器)
> + GCC: 4.4 及以上（建议gcc 4.8.1及以上）
> + Clang: 3.0 及以上 （建议 clang 3.4及以上）
> + VC: 10 及以上 （建议VC 12及以上）

+ [cmake](https://cmake.org/download/) 2.8.9 以上(建议 3.4 以上)
+ [msgpack](https://github.com/msgpack/msgpack-c)（用于协议打解包,仅使用头文件）
+ [libuv](http://libuv.org/)（用于网络通道）


Why not c?
------
本来像用纯c写这个组件的，因为纯c比较容易控制结构清晰和代码简洁，但是为什么之后又改用C++了呢？首先一个原因是初期准备使用的协议打解包组件msgpack是c++的；另一方面c++提供了比较简单好用的数据结构容器和内存管理组件，更重要的是这些东西都是原生跨平台的。所以就干脆用C++和C++风格来实现。

*PS:GCC都转依赖C++了我为嘛不能用？*

关于MsgPack
------
本来是想用flatbuffer做协议打解包的，因为使用flatbuffer的话不需要额外引入外部库。
但是实际使用过程中发现flatbuffer使用上问题有点多（比如有地址对齐问题，动态变更数据有缓冲区限制，string类型预分配了过多缓冲区等等）。

相比之下msgpack兼容性，使用容易程度都好很多。另外虽然我没有针对性的测试，但是据说msgpack的性能大约是protobuf的4倍。
而且如果支持c++03/c++11的话也可以使用纯header库，这是一大优势。当然它和protobuf一样存在打包和解包过程所以性能会低于flatbuffer，而且数据维护比较裸露不像protobuf采用了很多COW和ZeroCopy的技术。解包后的逻辑内存结构也会比较大。
但是考虑到在本lib中的应用，消息体的结构非常简单，并且附加信息很少，所以这些因素的影响都不是很大，反而CPU消耗和平台移植性显得更重要一些。

设计初衷和要点
------

1. **[扩展性]** 根据很多业务的需要，预留足够长的ID段（64位），用以给不同的ID段做不同类型的业务区分。
> 现有很多框架都是32位（比如腾讯的tbus和云风的[skynet](https://github.com/cloudwu/skynet)），在服务类型比较多的时候必须小心设计服务ID,以免冲突。
> 
> 当然也有考虑到以后可能会扩展为带Hash的字符串，所以在编译选项上做了预留。但是目前还是uint64_t

2. **[高性能]** 同物理机之间可以直接使用共享内存通信，大幅提高消息分发的性能。跨物理机之间会使用tcp通信。并且这个通信方式的选择是完全自动并透明的（尽可能选择更快的方式发送消息），业务层完全不需要关心。
3. **[跨平台]** 拥有不同习惯的Developer可以使用自己熟悉的工具，提高开发速度
4. **[动态路由]** 父子节点间会至少保持一条连接，自动断线重连。同父的兄弟节点之间完全按需建立连接。并且这个过程都是自动完成的，不需要提前初始化。
> 同样，新增节点和移除节点也不需要做什么特别的初始化操作。不会影响到已经在线上的服务。

5. **[低消耗]** 采用无锁队列，提高CPU性能。（共享）内存通道支持多端写入，一端读取，减少内存浪费。
> 如果N个节点两两互联，每个节点可以只拥有一个（共享）内存通道。即总共只有N个通道，内存消耗为N*每个通道内存占用
> 
> 一些其他的系统（比如tbus和我们目前的服务器框架）如果N个节点两两互联，每两个节点之间都要创建（共享）内存通道。即总共只有N*N个通道，内存消耗为N*N*每个通道内存占用。非常浪费

6. **[简化设计]** 根据一些实际的项目情况，把父子节点间的关系限定为Bus ID的后N位有包含关系，类似路由器的路由表的策略。
> 比如 0x12345678 可以控制的子节点有16位（0x12345678/16），那么0x12340000-0x1234FFFF都可以注册为它的子节点。
> 
> 如同IP协议中 192.168.1.1/24 路由表能管理 192.168.1.0-192.168.1.255 一样。当然这里的24指前24位，而前面提到的16指后16位。
> 
> 这么简化以后很多节点关系维护和通信都能简单很多并且能提高性能。

环境准备和构建流程
------
使用cmake标准构建方式，默认的编译目标为Debug版本，详见 [使用（编译）流程](doc/Build.md)

使用示例
------
简要的使用示例见 [使用示例](doc/Usage.md)

更加详细的请参考单元测试和[tools](tools)目录内的代码


Benchmark - Run On 2016-06-06
------
环境: CentOS 7.1, GCC 4.8.5
CPU: Xeon E3-1230 v2 (sender和receiver都只用一个核心)
内存: 24GB (这是总内存，具体使用数根据配置不同而不同)
网络: 千兆网卡 * 1
编译选项: -O2 -g -DNDEBUG -ggdb -Wall -Werror -Wno-unused-local-typedefs -std=gnu++11 -D_POSIX_MT_
配置选项: -DATBUS_MACRO_BUSID_TYPE=uint64_t -DATBUS_MACRO_CONNECTION_BACKLOG=128 -DATBUS_MACRO_CONNECTION_CONFIRM_TIMEOUT=30 -DATBUS_MACRO_DATA_ALIGN_TYPE=uint64_t -DATBUS_MACRO_DATA_NODE_SIZE=128 -DATBUS_MACRO_DATA_SMALL_SIZE=512 -DATBUS_MACRO_HUGETLB_SIZE=4194304 -DATBUS_MACRO_MSG_LIMIT=65536


                   测试项                |      连接数     |        包长度        |      CPU消耗     |    内存消耗   |    吞吐量     |      QPS
----------------------------------------|----------------|---------------------|-----------------|--------------|--------------|---------------
Linux+本地回环+ipv6+静态缓冲区             |         1      |      8-8096字节      |     97%/100%    |   20MB/5.5MB |    226MB/s   |    61.5K/s
Linux+本地回环+ipv6+静态缓冲区             |         1      | 8-128字节(模拟ping包) |     97%/100%    |   20MB/5.5MB |    9.8MB/s   |     170K/s
Linux+本地回环+ipv6+动态缓冲区(ptmalloc)   |         1      |      8-8096字节      |     95%/100%    |   28MB/5.5MB |    198MB/s   |    67.5K/s
Linux+本地回环+ipv6+动态缓冲区(ptmalloc)   |         1      | 8-128字节(模拟ping包) |     95%/100%    |   28MB/5.5MB |    8.3MB/s   |     159K/s
Linux+共享内存                           |         1      |      8-16384字节     |     100%/99%    |   36MB/36MB  |    --   |     --
Linux+共享内存                           |         1      | 8-128字节(模拟ping包) |     100%/99%    |   36MB/36MB  |    --   |     --


压力测试说明:

1. 动态缓冲区指发送者发送缓冲区使用malloc和free来保存发送中的数据,静态缓冲区指使用内置的内存池来缓存。
2. 由于发送者的CPU消耗会高于接收者（接收者没有任何逻辑，发送者会有一次随机数操作），所以动态缓冲区时其实内存会持续增加。（内存碎片原因，实际测试过程中3分钟由20MB涨到28MB）
3. Windows下除了不支持Unix Socket外，共享内存和ipv4/ipv6连接都是支持的，但是没有跑压力测试。

*PS: 目前没写多连接测试工具，后面有空再写吧*

支持工具
------
Linux下 GCC编译安装脚本(支持离线编译安装):

1. [GCC 5.X.X](https://github.com/owent-utils/bash-shell/tree/master/GCC%20Installer/gcc-5)
2. [LLVM & Clang](https://github.com/owent-utils/bash-shell/tree/master/LLVM%26Clang%20Installer)

LICENSE
------
+ libatbus 采用[MIT License](LICENSE)
+ MsgPack 采用[Boost Software License, Version 1.0协议](BOOST_LICENSE_1_0.txt)（类似MIT License）
+ libuv 采用[Node's license协议](NODE_S_LICENSE)（类似MIT License）