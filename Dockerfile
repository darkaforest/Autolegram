FROM darkaforest/autolegram:base
COPY . /usr/autolegram/
WORKDIR /usr/autolegram/
RUN rm -rf .git \
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
    && cp -rf ./autolegram/conf ./autolegram/tdlib/bin \
    && cp -rf ./autolegram/logconf ./autolegram/tdlib/bin \
    && mkdir -p ./autolegram/tdlib/bin/logs
ENV LANG zh_CN.UTF-8
WORKDIR /usr/autolegram/tdlib/bin/
CMD [ "java", "-Djava.library.path=.", "-Djava.util.logging.config.file=./logconf/logging.properties", "-Dfile.encoding=UTF-8", "org.darkaforest.tdlib.autolegram.Autolegram" ]
