ROOT=$(dirname $(realpath $0))

docker compose down --rmi local
sudo rm -f ${ROOT}/../images/u20s.qcow2
docker compose build
docker compose up -d --wait

${ROOT}/sh-guest.sh ${ROOT}/guest/mount-nvme.sh

${ROOT}/sh-guest.sh ${ROOT}/guest/install-postgre.sh
${ROOT}/sh-guest.sh ${ROOT}/guest/start-postgre.sh 0.01
