# compile csd-programs in docker
docker compose exec femu-csd \
    make -C /root/workspace/code/csd-programs > /dev/null 2>&1

# compile csd-passthru in guest
docker compose exec femu-csd ./run-guest-ssh.sh \
    make -C /home/femu/apps/csd-passthru > /dev/null 2>&1

# run passthru
docker compose exec femu-csd ./run-guest-ssh.sh \
    sudo /home/femu/apps/csd-passthru/csd-passthru $@