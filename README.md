# Autolegram - Telegram Auto Downloader

[![standard-readme compliant](https://img.shields.io/badge/readme%20style-standard-green.svg)](https://github.com/RichardLitt/standard-readme) [![license](https://img.shields.io/badge/license-BSL-lightgreen.svg)](https://www.boost.org/LICENSE_1_0.txt) [![platform](https://img.shields.io/badge/platform-docker-blue.svg)](https://hub.docker.com/repository/docker/darkaforest/autolegram)

[查看中文文档 Read Chinese Version](https://github.com/darkaforest/Autolegram/blob/main/README.zh-CN.md)

## Table of Contents

- [Background](#background)
- [Install](#install)
- [Usage](#usage)
- [Maintainers](#maintainers)
- [Contributing](#contributing)
- [Contributors](#Contributors)
- [License](#license)



## Background

Autolegram is a small, simply, easy tool to automatically download all kind of contents you received in telegram. Including photos, videos, files and so on, no matter in private chats, group chats or even channels.  It dose not has a gui and dose not going to have. It's recommend to run this tool in docker, but you could still run it everywhere as long as you can compile it on the target platform.



## Install

There are three ways to install autolegram.

- **Use docker image** 

    Easiest way to install, just use the docker image that has alreay been built. 

    ```shell
    $ docker pull darkaforest/autolegram:latest
    ```

    The image contains some geo-related settings like timezone, fonts, langs, etc. The default timezone has been set to Asia/Shanghai and lang set to zh_CN for my own convince, if you need a clean image without any geo-related settings, you can use `darkaforest/autolegram:latest-clean` to get clean image, which has every settings as default (and that usually means default to U.S.)

- **Build docker image**

    If you are not satisfied with the default Dockerfile or need adding your own stuff, just modify it in the way you want and build it by your own. The example shows below.

    ```shell
    $ git clone git@github.com:darkaforest/autolegram
    $ cd autolegram
    $ # make some change in Dockerfile
    $ docker build -t your-name/autolegram:latest .
    $ docker push your-name/autolegram:latest
    ```

- **Compile from source code**

    Autolegram is a Java project using tdlib native libs. In the case of compiling from source code, you need to compile tdlib first and then compile the lib's jni interface and finally java classes. It may take some time since the lib is pretty large (15min+ on my macbookpro 2017). And be aware of the platform you are using since tdlib is written in C/C++. Here's a example of comiling on CentOS 7, more examples could be found in  tdlib's offical [build generate tool](https://tdlib.github.io/td/build.html), but you need to adjust those paths to your own project.

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

    then run it using follow code

    ```shell
    $ cd /usr/autolegram/tdlib/bin/
    $ java -Djava.library.path=. -Djava.util.logging.config.file=./conf/logging.properties -Dfile.encoding=UTF-8 org.darkaforest.tdlib.autolegram.Autolegram
    ```



## Usage

As a auto downloader for telegram, it's obvious that autolegram need to know your telegram account infos, such as phone number, real time auth code, 2fa password. Those will be passsed to autolegram by config file named config.properties in `/usr/autolegram/tdlib/bin/conf/` , so you need map that file to docker container. The common config template can be found in [config folder](https://github.com/darkaforest/Autolegram/blob/main/conf/config.properties) in this project. 

The downloaded contents will be stored in `/usr/autolegram/tdlib/bin/output` and the logs contain all chat history and debug info will be stoed in `/usr/autolegram/tdlib/bin/logs`. You need to map those folder out to your host machine to get the contents.

For example, in most case, you can run the docker container as follow. Change the `host-path` to your path

```shell
$ docker run -d -v /host-path/conf/:/usr/autolegram/tdlib/bin/conf/ -v /host-path/logs/:/usr/autolegram/tdlib/bin/logs/ -v /host-path/output/:/usr/autolegram/tdlib/bin/output darkaforest/autolegram:latest
```

And the config.properties in your conf path should be like this.

```properties
# user phone number to login telegram user account, NOT bot account
telegram.user.phone=
# auth code, telegram will send you a code to verify login via sms or telegram
# put it here after received, it will be auto reload to continue process
# leave it empty before start
# if you've started multi rounds in a short time period, and not receiving new code, just use old one.
telegram.user.code=
# optional password. if you have set a password for login.
telegram.user.password=
# not much useful, please ignore this
telegram.user.email=
# not much useful, please ignore this
telegram.user.emailcode=

# api id and hash applied from telegram to identify one own client,
# it's okay to just leave default value below, which is copied from telegram official example, but not a good practice
telegram.client.apiid=94575
telegram.client.apihash=a3406de8d171bb422bb6ddf3bbd800e2


# client options, should be set same as you apply for apiid
# or leave it as-is if you want use default.
telegram.client.language=en
telegram.client.devicemodel=Desktop
telegram.client.version=1.0

# optional settings
# proxy server ip
proxy.server=
# proxy server port
proxy.port=
# optional username
proxy.username=
# optional password
proxy.password=
# proxy type should be one of [socks5, http, mtproto]
proxy.type=
# optional attribute for http proxy
proxy.httponly=
# must be set for mtproto proxy
proxy.secret=

# internal use only, do not change unless you know what you are doing
telegram.data.path=./output
telegram.data.checksum=true
telegram.data.enable=true
telegram.data.showdetail=false
telegram.data.maxchatsize=9999
```

There will be an auto generated file named `uniqeId.txt` in your `/host-path/output` folder, it is used for avoding redownload exists files, it is maintained by autolegram so do not touch it unless you know what your are doing.



## Maintainers

[@darkaforest](https://github.com/darkaforest)



## Contributing

Feel free to dive in! Open an issue or submit PRs.



## Contributors

This project exists thanks to all the people who contribute. 
[@darkaforest](https://github.com/darkaforest)



## License

[Boost Software License](https://www.boost.org/LICENSE_1_0.txt)