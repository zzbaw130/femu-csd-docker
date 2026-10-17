#!/bin/bash
set -eu

src_dir=/home/femu/apps/postgres
build_dir=/home/femu/opt/postgres/build
install_dir=/home/femu/opt/postgres/release

if command -v psql; then
    echo "postgre already installed, aborting."
    exit 0
fi

mkdir -p ${build_dir} && cd ${build_dir}

${src_dir}/configure --prefix=${install_dir}
make -j$(nproc)
make install

# install contrib extension
extensions=(pg_visibility)
for extension in ${extensions[@]}; do
    cd ${build_dir}/contrib/${extension}
    make -j$(nproc)
    make install
done

echo PATH="${install_dir}/bin:\$PATH" >> /home/femu/.profile

