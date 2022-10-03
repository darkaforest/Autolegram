FROM centos:centos7
COPY . /usr/autolegram/
WORKDIR /usr/autolegram/
RUN yum update -y \
    && yum install -y centos-release-scl-rh epel-release \
    && yum install -y devtoolset-9-gcc devtoolset-9-gcc-c++ \
    && yum install -y gcc-c++ make git zlib-devel openssl-devel php gperf cmake3 java-11-openjdk-devel \
    && sed -i '/override_install_langs/d' /etc/yum.conf \
    && yum groupinstall -y Fonts \
    && yum reinstall -y glibc-common \
    && fc-cache -fv \
    && localedef -c -f UTF-8 -i zh_CN zh_CN.UFT-8 \
    && echo "export LC_ALL=zh_CN.UTF-8" >> /etc/profile \
    && source /etc/profile \
    && ln -sf /usr/share/zoneinfo/Asia/Shanghai /etc/localtime \
    && echo 'Asia/Shanghai' > /etc/timezone \
    && rm -rf .git \
    && find . -name "*.DS_Store" | xargs rm -f \
    && rm -rf build \
    && mkdir build \
    && cd build \
    && CC=/opt/rh/devtoolset-9/root/usr/bin/gcc CXX=/opt/rh/devtoolset-9/root/usr/bin/g++ cmake3 -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX:PATH=../autolegram/java/td -DTD_ENABLE_JNI=ON .. \
    && cmake3 --build . --target install \
    && cd ../autolegram/java \
    && rm -rf build \
    && mkdir build \
    && cd build \
    && CC=/opt/rh/devtoolset-9/root/usr/bin/gcc CXX=/opt/rh/devtoolset-9/root/usr/bin/g++ cmake3 -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX:PATH=../../../tdlib -DTd_DIR:PATH=$(readlink -e ../td/lib/cmake/Td) .. \
    && cmake3 --build . --target install \
    && cd ../../../.. \
    && ls -l autolegram/tdlib \
    && cp -r ./autolegram/conf ./autolegram/tdlib/bin \
    && cp -r ./autolegram/logconf ./autolegram/tdlib/bin \
    && mkdir ./autolegram/tdlib/bin/logs
WORKDIR /usr/autolegram/tdlib/bin/
CMD [ "java", "-Djava.library.path=.", "-Djava.util.logging.config.file=./logconf/logging.properties", "-Dfile.encoding=UTF-8", "org.darkaforest.tdlib.autolegram.Autolegram" ]