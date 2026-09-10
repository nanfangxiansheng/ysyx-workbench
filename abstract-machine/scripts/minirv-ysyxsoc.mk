include $(AM_HOME)/scripts/minirv-npc.mk
image: image-dep
	cd $(AM_HOME)/../ysyxSoC/ready-to-run/minirv && bash gen.sh $(IMAGE).elf $(IMAGE).bin
