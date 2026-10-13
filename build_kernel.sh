#!/bin/bash
#complete Bandido kernel build script with LTO support
#the pgo support is unused right now

unset LLVM
#GCC SETUP
BUILD_CROSS_COMPILE=aarch64-linux-gnu-

#Enabling CLANG
export LLVM=1 LLVM_IAS=1

#setting toolchain path
ROOT_DIR="/home/me/kernelupgrade"
TC_DIR="$ROOT_DIR/toolchains/llvm-23.1.0-x86_64"
export PATH="$TC_DIR/bin:$PATH"

##########################################################
mkdir -p out
#rm out/dump*

export ARCH=arm64
CLANG_TRIPLE=aarch64-linux-gnu

CPU=$(($(nproc) - 1))
DATE_START=$(date +"%s")
IMAGE="out/arch/arm64/boot/Image.gz-dtb"

if [[ $1 != "flash" ]]; then
	#Remove a previous kernel image
	rm out/arch/arm64/boot/Image* &>/dev/null

	make -j$CPU -C $(pwd) O=$(pwd)/out ARCH=arm64 CROSS_COMPILE=$BUILD_CROSS_COMPILE CLANG_TRIPLE=$CLANG_TRIPLE bandido_defconfig

	#Remove "=y" or "is not set"
	scripts/configcleaner "CONFIG_LTO_CLANG
		CONFIG_THINLTO
		CONFIG_LTO_GCC
		CONFIG_PGO_CLANG
		CONFIG_PGOUSE_CLANG"

	if [[ -v LLVM ]]; then
		echo -e "# CONFIG_LTO_GCC is not set\n" >>out/.config
		case $1 in
		lto)
			echo -e "\n################# Compiling FULL CLANG LTO build #################\n"
			echo -e "# CONFIG_THINLTO is not set\n" >>out/.config
			echo -e "# CONFIG_PGOUSE_CLANG is not set\n" >>out/.config
			echo -e "# CONFIG_PGO_CLANG is not set\n" >>out/.config
			echo -e "CONFIG_LTO_CLANG=y\n" >>out/.config
			;;
		pgo)
			echo -e "\n################# Compiling FULL CLANG LTO PGO build #################\n"
			echo -e "# CONFIG_THINLTO is not set\n" >>out/.config
			echo -e "CONFIG_PGO_CLANG=y\n" >>out/.config
			echo -e "# CONFIG_PGOUSE_CLANG is not set\n" >>out/.config
			echo -e "# CONFIG_LTO_CLANG is not set\n" >>out/.config
			;;
		pgouse)
			echo -e "\n################# Compiling FULL CLANG LTO PGOUSE build #################\n"
			echo -e "# CONFIG_THINLTO is not set\n" >>out/.config
			echo -e "# CONFIG_PGO_CLANG is not set\n" >>out/.config
			echo -e "CONFIG_PGOUSE_CLANG=y\n" >>out/.config
			;;
		*)
			echo -e "CONFIG_THINLTO=y\n" >>out/.config
			echo -e "# CONFIG_PGOUSE_CLANG is not set\n" >>out/.config
			echo -e "# CONFIG_PGO_CLANG is not set\n" >>out/.config
			echo -e "CONFIG_LTO_CLANG=y\n" >>out/.config
			;;
		esac
	else
		case $1 in
		lto)
			echo -e "\n################# Compiling FULL GCC LTO build #################\n"
			echo -e "\nCONFIG_LTO_GCC=y\n" >>out/.config
			;;

		*)
			echo -e "\n# CONFIG_LTO_GCC is not set\n" >>out/.config
			;;
		esac
	fi

	make -j$CPU -C $(pwd) O=$(pwd)/out ARCH=arm64 CROSS_COMPILE=$BUILD_CROSS_COMPILE \
		CLANG_TRIPLE=$CLANG_TRIPLE oldconfig

	make -j$CPU -C $(pwd) O=$(pwd)/out ARCH=arm64 CROSS_COMPILE=$BUILD_CROSS_COMPILE \
		CLANG_TRIPLE=$CLANG_TRIPLE Image.gz-dtb 2>&1 | tee compile-bandido.log
fi


if [[ -f "$IMAGE" ]]; then
	DATE_END=$(date +"%s")
	DIFF=$(($DATE_END - $DATE_START))

	KERNELZIP="bandido-$(date +"%Y%m%d%H%M").zip"

	rm AnyKernel3/dtb* >/dev/null 2>&1
	rm AnyKernel3/*Image* >/dev/null 2>&1
	rm AnyKernel3/*.zip >/dev/null 2>&1

	# Manual DTBO packaging using official script
	DTBO_FILES=$(find out/arch/arm64/boot/dts/samsung/ -name "*.dtbo" | sort)
	if [[ -n "$DTBO_FILES" ]]; then
		echo -e "\nPackaging DTBO image with mkdtboimg.py...\n"
		python3 scripts/mkdtboimg.py create AnyKernel3/dtbo.img --page_size=4096 $DTBO_FILES
	fi

	cp $IMAGE AnyKernel3/
	cat out/arch/arm64/boot/dts/vendor/qcom/kona.dtb \
		out/arch/arm64/boot/dts/vendor/qcom/kona-v2.dtb \
		out/arch/arm64/boot/dts/vendor/qcom/kona-v2.1.dtb > AnyKernel3/dtb

	cd AnyKernel3

	zip -r9 $KERNELZIP . -x ".git*" ".github*" "README.md" "*placeholder"

	echo -e "\nTime elapsed: $(($DIFF / 60)) minute(s) and $(($DIFF % 60)) seconds.\n"

	#save a copy
	cp -v $KERNELZIP ~/build

	#this will wait for the device to try to flash automatically
	while true; do
		adb start-server >/dev/null 2>&1
		STATE=$(adb get-state 2>/dev/null)

		if [[ $STATE == "device" ]]; then
			echo "Device is active normally. Rebooting to recovery..."
			adb reboot recovery
			sleep 45
		elif [[ $STATE == "recovery" ]]; then
			echo "Device is in recovery mode."
			break
		else
			echo "No device found in device or recovery state. Retrying in 5 seconds..."
			sleep 5
		fi
	done

	# Wait for Recovery (TWRP or OrangeFox) to be fully ready before pushing/installing
	echo "Waiting for Recovery (TWRP / OrangeFox) to be fully ready..."
	while ! adb shell "twrp help 2>&1 || fox help 2>&1" | grep -qE "TWRP|OrangeFox"; do
		sleep 2
	done
	
	echo "Allowing TWRP to stabilize..."
	sleep 5

	echo "Pushing kernel zip to device..."
	PUSH_SUCCESS=0
	for i in {1..5}; do
		if adb push "$KERNELZIP" /tmp/; then
			PUSH_SUCCESS=1
			break
		fi
		echo "Push failed, retrying in 3 seconds ($i/5)..."
		sleep 3
	done
	if [[ $PUSH_SUCCESS -ne 1 ]]; then
		echo "ERROR: Failed to push kernel zip to device!"
		exit 1
	fi

	echo "Installing kernel via TWRP/OrangeFox..."
	INSTALL_LOG=$(mktemp)
	
	# Try installing up to 3 times in case of silent failure
	INSTALL_SUCCESS=0
	for i in {1..3}; do
		adb shell "twrp install /tmp/$KERNELZIP" 2>&1 | tee "$INSTALL_LOG"
		
		# Check if it failed or if the output was completely empty (silent failure)
		if grep -qE "Error installing|Unable to locate|Failed" "$INSTALL_LOG"; then
			echo "Installation failed, retrying ($i/3)..."
			sleep 3
		elif [ ! -s "$INSTALL_LOG" ] || ! grep -q "Done" "$INSTALL_LOG"; then
			echo "Silent failure (TWRP busy?), retrying ($i/3)..."
			sleep 3
		else
			INSTALL_SUCCESS=1
			break
		fi
	done
	
	rm -f "$INSTALL_LOG"
	
	if [[ $INSTALL_SUCCESS -ne 1 ]]; then
		echo "ERROR: Failed to install kernel via TWRP/OrangeFox after 3 attempts!"
		exit 1
	fi
	echo -e "\a"
	echo "Installation completed! Rebooting to system..."
	sleep 5
	adb shell "twrp reboot"
else
	echo -e "\nERROR. Something broke along the way since $IMAGE is not there\n"
fi
