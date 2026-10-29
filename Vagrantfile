# -*- mode: ruby -*-
# vi: set ft=ruby :
#
# Windows에서 vagrant up으로 Ubuntu 22.04 개발 VM을 실행한다.
# 필요 패키지도 설치한다. 사용 방법은 docs/04-setup-windows.md를 참고한다.

Vagrant.configure("2") do |config|
  config.vm.box      = "ubuntu/jammy64"
  config.vm.hostname = "zns-dev"

  config.vm.provider "virtualbox" do |vb|
    vb.name   = "dm-zns-base"
    vb.memory = 4096
    vb.cpus   = 2
  end

  # 저장소 루트를 게스트 /vagrant에 동기화한다.
  # provision.sh는 패키지만 설치한다. null_blk 생성은 scripts/nullblk-up.sh가 담당한다.
  # null_blk는 메모리 기반이므로 게스트 재부팅 뒤 다시 생성해야 한다.
  config.vm.provision "shell", path: "provision.sh"
end
