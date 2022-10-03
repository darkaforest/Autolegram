# Autolegram - Telegram 自动下载器

[![standard-readme compliant](https://img.shields.io/badge/readme%20style-standard-green.svg)](https://github.com/RichardLitt/standard-readme) [![license](https://img.shields.io/badge/license-BSL-lightgreen.svg)](https://www.boost.org/LICENSE_1_0.txt) [![platform](https://img.shields.io/badge/platform-docker-blue.svg)](https://hub.docker.com/repository/docker/darkaforest/autolegram)



## 目录

- [项目背景](#项目背景)
- [安装](#安装)
- [使用](#使用)
- [作者](#作者)
- [参与项目](#参与项目)
- [项目成员](#项目成员)
- [开源许可](#开源许可)



## 项目背景

​		Autolegram 是一个用来自动下载 Telegram 全量内容的小型、极简、易于使用的工具。下载内容包括但不限于图片、视频、文件，无论是通过私聊、群聊还是频道，都会被自动下载。Autolegram 不是一款图形化客户端，也不会成为图形化客户端。推荐使用 docker 运行，不过实际上你可以在任何能成功通过编译的平台架构上使用。



## 安装

​		autolegram 可以通过3种方式安装。 

- **直接使用 docker 镜像**

    ​		最简单的方法，直接使用已经构建好的最新 docker 镜像。 

    ```shell
    $ docker pull darkaforest/autolegram:latest
    ```

    ​		此镜像包含一些区域化设置，比如时区、语言、字体等。为了方便为我自己使用，默认设置时区为上海，语言为简中。如果你需要没有区域化设置的镜像，可以使用 `darkaforest/autolegram:latest-clean` 镜像。这个镜像里所有东西都是默认值（一般来讲就是默认值就是美国值）。

- **自行构建 docker 镜像**

    ​		如果默认的 Dockerfile 不能满足你的需求，或者你需要添加自己的区域设置，可以直接修改项目中的 Dockerfile 并使用以下代码构建。

    ```shell
    $ git clone git@github.com:darkaforest/autolegram
    $ cd autolegram
    $ # make some change in Dockerfile
    $ docker build -t your-name/autolegram:latest .
    $ docker push your-name/autolegram:latest
    ```

- **源码编译**

    ​		autolegram 是 Java 项目，但是其使用的 tdlib 库是系统相关的本地库。如果一定要从源码编译的话，你需要先编译 tdlib，然后编译 tdlib 的 JNI 接口，最后才能把 Java 文件编译成 class 文件。考虑到 tdlib 是个挺大的库，编译起来可能会需要不短的时间（我在自己的 macbookpro 2017 上至少要花15分钟）。而且 tdlib 是个 C/C++ 的库，一定要注意你编译的平台和实际运行的平台是否一致。下文给出了在 CentOS 7 上编译的完整示例。更多示例可以在 tdlib 的官方[构建脚本生成器](https://tdlib.github.io/td/build.html)找到，但是里面的路径需要按你自己的实际情况修改。

    ```shell
    $ sudo yum update -y
    $ sudo yum install -y centos-release-scl-rh epel-release
    $ sudo yum install -y devtoolset-9-gcc devtoolset-9-gcc-c++
    $ sudo yum install -y gcc-c++ make git zlib-devel openssl-devel php gperf cmake3 java-11-openjdk-devel
    $ git clone git@github.com:darkaforest/autolegram
    $ cd autolegram
    $ rm -rf .git
    $ rm -rf build
    $ mkdir build
    $ cd build
    $ CC=/opt/rh/devtoolset-9/root/usr/bin/gcc CXX=/opt/rh/devtoolset-9/root/usr/bin/g++ cmake3 -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX:PATH=../autolegram/java/td -DTD_ENABLE_JNI=ON ..
    $ cmake3 --build . --target install
    $ cd ..
    $ cd autolegram/java
    $ rm -rf build
    $ mkdir build
    $ cd build
    CC=/opt/rh/devtoolset-9/root/usr/bin/gcc CXX=/opt/rh/devtoolset-9/root/usr/bin/g++ cmake3 -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX:PATH=../../../tdlib -DTd_DIR:PATH=$(readlink -e ../td/lib/cmake/Td) ..
    $ cmake3 --build . --target install
    $ cd ../../..
    $ cd ..
    $ ls -l autolegram/tdlib
    $ cp -r ./autolegram/conf ./autolegram/tdlib/bin
    $ mkdir ./autolegram/tdlib/bin/logs
    ```
    
    ​		然后使用如下代码运行。
    
    ```shell
    $ cd /usr/autolegram/tdlib/bin/
    $ java -Djava.library.path=. -Djava.util.logging.config.file=./conf/logging.properties -Dfile.encoding=UTF-8 org.darkaforest.tdlib.autolegram.Autolegram
    ```



## 使用

​		作为一个 telegram 自动下载器，显然 autolegram 需要知道你的 telegram 账号信息，比如手机号、实时验证码、双因子密码。这些信息通过在 `/usr/autolegram/tdlib/bin/conf/` 下的 `config.properties` 配置文件传递给应用程序。所以需要手动把配置文件映射到这个路径。配置文件的通用模板在本项目的 [config 文件夹](https://github.com/darkaforest/Autolegram/blob/main/conf/config.properties)。

​		已下载内容会被存储到 `/usr/autolegram/tdlib/bin/output` ，包含全量聊天历史和诊断信息的日志会被存储到 `/usr/autolegram/tdlib/bin/logs`。这两个文件夹需要被映射到宿主机来获取下载内容。

​		如下所示，一般来说，可以通过这样的命令来启动容器。`host-path` 需要改为你自己的路径。需要是绝对路径。

```shell
$ docker run -d -v /host-path/conf/:/usr/autolegram/tdlib/bin/conf/ -v /host-path/logs/:/usr/autolegram/tdlib/bin/logs/ -v /host-path/output/:/usr/autolegram/tdlib/bin/output darkaforest/autolegram:latest
```

​		你放在 `/host-path/conf/` 文件夹下的 `config.properties` 文件应该如下所示。

```properties
# 用来登录的用户手机号，不是bot哦
telegram.user.phone=
# 验证码，登录的时候 telegram 会发给你，可能是通过短信或消息。
# 收到验证码后填到这里，立刻保存文件。这个值会被自动读取
# 启动之前这里留空。
# 短时间多次启动的情况，如果没有收到新的验证码，这里就填最后一次收到的验证码
telegram.user.code=
# 如果设置了密码，填在这里
telegram.user.password=
# 没什么卵用
telegram.user.email=
# 没什么卵用
telegram.user.emailcode=

# apiid 和 apihash 是用来标识唯一客户端的属性，可以在 telegram 申请
# 理论上每个使用者都应该申请自己的，但是就用下面这个默认值也可以，这是从官方示例复制过来的。
# 但是请尽量力所能及的申请自己的apiid
telegram.client.apiid=94575
telegram.client.apihash=a3406de8d171bb422bb6ddf3bbd800e2


# 客户端属性，填写申请时的值，没有的话就用下面默认的
telegram.client.language=en
telegram.client.devicemodel=Desktop
telegram.client.version=1.0

# 可选设置
# 代理服务器IP
proxy.server=
# 代理服务器端口
proxy.port=
# 代理服务器用户名（可选）
proxy.username=
# 代理服务器密码（可选）
proxy.password=
# 代理类型，[socks5, http, mtproto] 三选一，不区分大小写
proxy.type=
# http 代理可以设置这个
proxy.httponly=
# mtproto 代理必须设置这个
proxy.secret=

# 内部配置，不懂勿动
telegram.data.path=./output
telegram.data.checksum=true
telegram.data.enable=true
telegram.data.showdetail=false
telegram.data.maxchatsize=9999
```

​		在 `/host-path/output` 文件夹里面会有个自动生成的文件叫 `uniqeId.txt` ，是用来避免重复下载已存在文件的，这个文件由 autolegram 创建和维护，如无特殊需要别动它就好。



## 作者

[@darkaforest](https://github.com/darkaforest)



## 参与项目

欢迎提交 PR 和 Issue。



## 项目成员

感谢以下项目成员做出的贡献！ 
[@darkaforest](https://github.com/darkaforest)



## 开源许可

[Boost Software License](https://www.boost.org/LICENSE_1_0.txt)