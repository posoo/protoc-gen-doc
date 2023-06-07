# Build the execution
From ubuntu:focal AS Build

WORKDIR /src
COPY . /src

RUN apt update
RUN DEBIAN_FRONTEND=noninteractive apt install -y qt5-qmake qt5-default qtbase5-dev libprotobuf-dev libprotoc-dev lua5.3 liblua5.3-dev g++ make
RUN qmake
RUN make


# Pack the execution
FROM ubuntu:focal

RUN apt update
RUN DEBIAN_FRONTEND=noninteractive apt install -y libqt5core5a libprotoc-dev protobuf-compiler

COPY LICENSE.md README.md entrypoint.sh /
COPY --from=build /src/protoc-gen-doc /usr/bin/

VOLUME ["/out"]
VOLUME ["/protos"]
WORKDIR /protos

ENTRYPOINT ["/entrypoint.sh"]
CMD ["--doc_opt=html,index.html"]

