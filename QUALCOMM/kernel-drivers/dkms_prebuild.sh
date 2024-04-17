#!/usr/bin/env bash
kernver=$1
std_kerndir=$2
source_tree=$3

config_file="$std_kerndir/.config"
modsyms_file="$std_kerndir/Module.symvers"

#the subsequent workaround is relevant in production
#test env always points to the correct kerndir and doesn't pass source_tree
if [ -n "$source_tree" ] ; then
#SUSE puts its include dir in a separate location
#(see /usr/share/doc/packages/kernel-source/README.SUSE)
. /etc/os-release
kerndir=$([[ "$ID_LIKE" =~ "suse" ]] && echo "$source_tree/linux-${kernver%-*}" || echo "$std_kerndir")
else
kerndir=$std_kerndir
fi

inc_drm="$kerndir/include/drm"
inc_linux="$kerndir/include/linux"
inc_uapi="$kerndir/include/uapi/linux"
BACKPORT_FLAGS=( )

echo "Checking if backports are needed for:"
echo "kernel version = $kernver"
echo "kerneldir = $kerndir"
echo "config file = $config_file"
echo "modsyms_file = $modsyms_file"

if [[ ! -f $config_file ]] ; then
	echo "WARNING: $config_file does not exist"
fi

if [[ ! -f $modsyms_file ]] ; then
	echo "WARNING: $modsyms_file does not exist"
fi

function en_flag {
	local FLAG=$1
	BACKPORT_FLAGS+=("-D$FLAG")
}

function grab_struct {
	local struct_name=$1
	local file=$2
	local struct=""

	if [ -f "$file" ] ; then
		#first find a line that matches the pattern '(beginning of line)struct struct_name {'
		#then print every line until the pattern '(beginning of line)};(end of line)'
		struct=$(sed -n "/^struct $struct_name {/,/^};$/p" "$file")
	fi
	echo "$struct"
}

function chk_file {
	local check_for="$1"
	local file="$2"
	local grepped=""
	local output=""

	#escape '*'
	check_for=${check_for//\*/\\*}

	if [ -f "$file" ] ; then
		grepped=$(grep "$check_for" "$file" | tr -d '\t' | tr '\n' '@')
		if [ -n "$grepped" ] ; then
			echo -n "$grepped"
			output=$(grep -c "$check_for" "$file")
		fi
	fi
	echo "$output"
}

function chk_struct {
	local check_for="$1"
	local struct_name="$2"
	local file="$3"
	local output=""
	local struct
	local grepped
	local count

	#escape '*'
	check_for=${check_for//\*/\\*}

	struct=$(grab_struct "$struct_name" "$file")
	grepped=$(echo "$struct" | grep "$check_for")
	#need to redo work
	#$(echo "$grepped" | wc -1) - will always be at least 1 since trailing \n is added
	#$(echo -n "$grepped" | wc -l) - will report 1 found match as 0 since trailing \n is removed
	count=$(echo "$struct" | grep -c "$check_for")
	if [ -n "$struct" ] && [ "$count" != 0 ] ; then
		grepped=$(echo "$grepped" | tr -d '\t'| tr '\n' '@')
		echo -n "$grepped"
		output="$count"
	fi
	echo "$output"
}

function chk_few_files {
	local file_a=$1
	shift
	local file_b=$1
	shift
	local chk_type=$1
	shift
	local chk_func=$1
	shift
	local chk_args=("$@")
	local check_a=""
	local check_b=""
	local output=""

	if [[ "$chk_func" != "chk_file" && "$chk_func" != "chk_struct" ]] ; then
		echo "$chk_func is not a valid check function"
		exit 255
	fi

	check_a=$($chk_func "${chk_args[@]}" "$file_a")
	check_b=$($chk_func "${chk_args[@]}" "$file_b")

	case "$chk_type" in
	  "+")
		if [ -n "$check_a" ] || [ -n "$check_b" ] ; then
			#if found in both we have other problems...
			output='y'
		fi
		;;
	  "-")
		if [ -z "$check_a" ] && [ -z "$check_b" ] ; then
			output='y'
		fi
		;;
	  *)
		echo "'$chk_type' is not a valid check type"
		exit 255
	esac
	echo "$output"
}

## _QBP_ALT_DRM_GEM_OBJ_FUNCS ##
#defined if 'drm_gem_object' struct does NOT contain 'drm_gem_object_funcs'
ENABLE=$(chk_struct "struct drm_gem_object_funcs" "drm_gem_object" "$inc_drm/drm_gem.h")
[ -z "$ENABLE" ] && en_flag "_QBP_ALT_DRM_GEM_OBJ_FUNCS"

## _QBP_ALT_DRM_GEM_OBJ_MMAP_FUNC ##
#defined if 'drm_gem_object_funcs' struct does NOT have 'mmap' function pointer
ENABLE=$(chk_struct "(*mmap)" "drm_gem_object_funcs" "$inc_drm/drm_gem.h")
[ -z "$ENABLE" ] && en_flag "_QBP_ALT_DRM_GEM_OBJ_MMAP_FUNC"

## _QBP_ALT_DRM_MANAGED ##
#defined if 'drm_managed.h' does NOT exist
[ ! -f "$inc_drm/drm_managed.h" ] && en_flag "_QBP_ALT_DRM_MANAGED"

## _QBP_ALT_DRM_MANAGED_NO_RELEASE ##
#defined if drm_driver struct has no (*release) callback
ENABLE=$(chk_few_files "$inc_drm/drmP.h" "$inc_drm/drm_drv.h" "-" "chk_struct" "(*release)" "drm_driver")
[ -n "$ENABLE" ] && en_flag "_QBP_ALT_DRM_MANAGED_NO_RELEASE"

## _QBP_HAS_CHECK_OVERFLOW ##
#defined if overflow.h exists
[ -f "$inc_linux/overflow.h" ] && en_flag "_QBP_HAS_CHECK_OVERFLOW"

## _QBP_HAS_CLASS_CREATE_ONE_ARG ##
#defined if include/linux/device/class.h has 'class_create(const char *name)'
ENABLE=$(chk_file "class_create(const char *name)" "$inc_linux/device/class.h")
[ -n "$ENABLE" ] && en_flag "_QBP_HAS_CLASS_CREATE_ONE_ARG"

## _QBP_HAS_CONST_CLASS_DEVNODE ##
#defined if struct class has devnode with const device
ENA=$(chk_few_files "$inc_linux/device/class.h" "$inc_linux/device.h" "+" "chk_struct" "devnode)(const struct device" "class")
BLE=$(chk_few_files "$inc_linux/device/class.h" "$inc_linux/device.h" "+" "chk_struct" "devnode)(RH_KABI_CONST struct device" "class")
[ -n "$ENA" ] || [ -n "$BLE" ] && en_flag "_QBP_HAS_CONST_CLASS_DEVNODE"

## _QBP_HAS_CONST_DRM_DRIVER ##
#define if in include/drm/drm_drv.h, drm_dev_alloc takes a const driver
ENABLE=$(chk_file "\*drm_dev_alloc(const struct drm_driver" "$inc_drm/drm_drv.h")
[ -n "$ENABLE" ] && en_flag "_QBP_HAS_CONST_DRM_DRIVER"

## _QBP_HAS_DRM_FILE_EVENT_INFO_LOCK ##
#pre 4.12 - check include/drm/drmP.h
#after 4.12 - check include/drm/drm_file.h
#defined if struct 'drm_file' has 'mutex event_read_lock'
ENABLE=$(chk_few_files "$inc_drm/drmP.h" "$inc_drm/drm_file.h" "+" "chk_struct" "mutex event_read_lock" "drm_file")
[ -n "$ENABLE" ] && en_flag "_QBP_HAS_DRM_FILE_EVENT_INFO_LOCK"

## _QBP_HAS_DRM_FILE_LOOKUP_LOCK ##
#defined include/drm/drm_file.h if struct 'drm_file' has 'master_lookup_lock'
ENABLE=$(chk_struct "master_lookup_lock" "drm_file" "$inc_drm/drm_file.h")
[ -n "$ENABLE" ] && en_flag "_QBP_HAS_DRM_FILE_LOOKUP_LOCK"

## _QBP_HAS_DRM_GEM_OBJ_DMA_RESV ##
#defined if include/drm/drm_gem.h's struct drm_gem_object has member 'struct dma_resv *resv'
ENABLE=$(chk_struct "struct dma_resv *resv" "drm_gem_object" "$inc_drm/drm_gem.h")
[ -n "$ENABLE" ] && en_flag "_QBP_HAS_DRM_GEM_OBJ_DMA_RESV"

## _QBP_HAS_DRM_DRV_DEBUGFS_CLEANUP ##
#defined if drm_driver has (*debugfs_cleanup) callback
ENABLE=$(chk_few_files "$inc_drm/drmP.h" "$inc_drm/drm_drv.h" "+" "chk_struct" "void (*debugfs_cleanup)" "drm_driver")
[ -n "$ENABLE" ] && en_flag "_QBP_HAS_DRM_DRV_DEBUGFS_CLEANUP"

## _QBP_HAS_DRM_DRV_GEM_PRINT_INFO ##
#defined if struct 'drm_driver' has 'gem_print_info' function pointer
ENABLE=$(chk_struct "(*gem_print_info)" "drm_driver" "$inc_drm/drm_drv.h")
[ -n "$ENABLE" ] && en_flag "_QBP_HAS_DRM_DRV_GEM_PRINT_INFO"

## _QBP_HAS_DRM_GEM_PRIME_MMAP_FUNC ##
#defined if function drm_gem_prime_mmap() exists
ENABLE=$(chk_file "drm_gem_prime_mmap" "$inc_drm/drm_prime.h")
[ -n "$ENABLE" ] && en_flag "_QBP_HAS_DRM_GEM_PRIME_MMAP_FUNC"

## _QBP_HAS_DRM_DRV_WITH_GEM_PRIME_MMAP ##
#defined if struct drm_driver has member "int (*gem_prime_mmap)()"
ENABLE=$(chk_few_files "$inc_drm/drmP.h" "$inc_drm/drm_drv.h" "+" "chk_struct" "(*gem_prime_mmap)" "drm_driver")
[ -n "$ENABLE" ] && en_flag "_QBP_HAS_DRM_DRV_WITH_GEM_PRIME_MMAP"

## _QBP_HAS_DRM_DRV_WITH_GEM_PRIME_GET_SG_TABLE ##
#defined if struct drm_driver has member "struct sg_table *(*gem_prime_get_sg_table)()"
ENABLE=$(chk_few_files "$inc_drm/drmP.h" "$inc_drm/drm_drv.h" "+" "chk_struct" "(*gem_prime_get_sg_table)" "drm_driver")
[ -n "$ENABLE" ] && en_flag "_QBP_HAS_DRM_DRV_WITH_GEM_PRIME_GET_SG_TABLE"

## _QBP_HAS_INCLUSIVE_MAX_ORDER ##
#check include/linux/mmzone.h that "define MAX_ORDER_NR_PAGES (1 << MAX_ORDER)
#when MAX_ORDER is not inclusive it has 1 subtracted before it's used.
ENABLE=$(chk_file "define MAX_ORDER_NR_PAGES (1 << MAX_ORDER)" "$inc_linux/mmzone.h")
[ -n "$ENABLE" ] && en_flag "_QBP_HAS_INCLUSIVE_MAX_ORDER"

## _QBP_HAS_INT_DEBUGFS_INIT_RETVAL ##
#pre 4.10 - include/drm/drmP.h
#post 4.10 - include/drm/drm_drv.h
#defined if struct 'drm_driver' has 'int (*debugfs_init)'
ENABLE=$(chk_few_files "$inc_drm/drmP.h" "$inc_drm/drm_drv.h" "+" "chk_struct" "int (*debugfs_init)" "drm_driver")
[ -n "$ENABLE" ] && en_flag "_QBP_HAS_INT_DEBUGFS_INIT_RETVAL"

## _QBP_HAS_MOD_IMPORT_DMA_BUF ##
ENABLE=$(chk_file "\sDMA_BUF" "$modsyms_file")
[ -n "$ENABLE" ] && en_flag "_QBP_HAS_MOD_IMPORT_DMA_BUF"

## _QBP_HAS_PCI_ERROR_RESET_NOTIFY ##
#check include/linux/pci.h 'pci_error_handlers' struct if '(*reset_notify)'
ENABLE=$(chk_struct "(*reset_notify)" "pci_error_handlers" "$inc_linux/pci.h")
[ -n "$ENABLE" ] && en_flag "_QBP_HAS_PCI_ERROR_RESET_NOTIFY"

## _QBP_HAS_PCI_ERROR_RESET_PREPDONE ##
#defined if include/linux/pci.h if struct 'pci_error_handlers' has 'void (*reset_prepare)' 'void (*reset_done)'
ENA=$(chk_struct "void (*reset_prepare)" "pci_error_handlers" "$inc_linux/pci.h")
BLE=$(chk_struct "void (*reset_done)" "pci_error_handlers" "$inc_linux/pci.h")
[ -n "$ENA" ] && [ -n "$BLE" ] && en_flag "_QBP_HAS_PCI_ERROR_RESET_PREPDONE"

## _QBP_HAS_PRANDOM_U32 ##
#defined if include/linux/random.h has get_random_u32_inclusive
ENABLE=$(chk_file "get_random_u32_inclusive" "$inc_linux/random.h")
[ -n "$ENABLE" ] && en_flag "_QBP_HAS_PRANDOM_U32"

## _QBP_HAS_UEVENT_CONST_DEV ##
#defined if include/linux/device/bus.h that struct 'bus_type' has element '(*uevent)(const struct device'
ENABLE=$(chk_struct "(*uevent)(const struct device" "bus_type" "$inc_linux/device/bus.h")
[ -n "$ENABLE" ] && en_flag "_QBP_HAS_UEVENT_CONST_DEV"

## _QBP_HAS_XARRAY_STABLE_XA_ALLOC ##
#check include/linux/xarray.h if 'int xa_alloc(' doesn't have 'u32 max' on same line (or max before entry in func def)
#defined if NOT found OR 'max' is a variable
ENABLE=$(chk_file "int xa_alloc(" "$inc_linux/xarray.h")
FILTERED=${ENABLE/u32 max,/FAIL,}
[ -z "$ENABLE" ] || [ "$ENABLE" != "$FILTERED" ] && en_flag "_QBP_NEED_XARRAY_STABLE_XA_ALLOC"

## _QBP_INCLUDE_BITOPS ##
#defined if bits.h does NOT exist
[ ! -f "$inc_linux/bits.h" ] && en_flag "_QBP_INCLUDE_BITOPS"

## _QBP_INCLUDE_BITOPS_MASK ##
#defined if bitops.h does NOT exist
[ ! -f "$inc_linux/bitops.h" ] && en_flag "_QBP_INCLUDE_BITOPS_MASK"

## _QBP_INCLUDE_DRMP ##
#defined if 'drm_core_check_feature' or 'DRM_SWITCH_POWER_ON' is defined
ENA=$(chk_file "bool drm_core_check_feature" "$inc_drm/drmP.h")
BLE=$(chk_file "define DRM_SWITCH_POWER_ON" "$inc_drm/drmP.h")
[ -n "$ENA" ] || [ -n "$BLE" ] && en_flag "_QBP_INCLUDE_DRMP"

## _QBP_INCLUDE_FIX_DRM_DEBUGFS ##
#defined if 'drm_debugfs.h' does NOT include 'seq_file.h'
ENABLE=$(chk_file "include <linux/seq_file.h>" "$inc_drm/drm_debugfs.h")
[ -z "$ENABLE" ] && en_flag "_QBP_INCLUDE_FIX_DRM_DEBUGFS"

## _QBP_INCLUDE_FIX_DRM_FILE ##
#defined if 'drm_file.h' does NOT include 'linux/idr.h'
ENABLE=$(chk_file "include <linux/idr.h>" "$inc_drm/drm_file.h")
[ -z "$ENABLE" ] && en_flag "_QBP_INCLUDE_FIX_DRM_FILE"

## _QBP_INCLUDE_FIX_DRM_GEM ##
#defined if drm_gem.h does NOT have drm_vma_manager.h included
ENABLE=$(chk_file "include <drm/drm_vma_manager.h>" "$inc_drm/drm_gem.h")
[ -z "$ENABLE" ] && en_flag "_QBP_INCLUDE_FIX_DRM_GEM"

## _QBP_INCLUDE_HAS_DRM_DEBUGFS ##
#defined if include/drm/drm_debugfs.h exists
[ -f "$inc_drm/drm_debugfs.h" ] && en_flag "_QBP_INCLUDE_HAS_DRM_DEBUGFS"

## _QBP_INCLUDE_HAS_DRM_DEVICE ##
#defined if include/drm/drm_device.h exists
[ -f "$inc_drm/drm_device.h" ] && en_flag "_QBP_INCLUDE_HAS_DRM_DEVICE"

## _QBP_INCLUDE_HAS_DRM_DRV ##
#defined if include/drm/drm_drv.h exists
[ -f "$inc_drm/drm_drv.h" ] && en_flag "_QBP_INCLUDE_HAS_DRM_DRV"

## _QBP_INCLUDE_HAS_DRM_FILE ##
#defined if include/drm/drm_file.h exists
[ -f "$inc_drm/drm_file.h" ] && en_flag "_QBP_INCLUDE_HAS_DRM_FILE"

## _QBP_INCLUDE_HAS_DRM_IOCTL ##
#defined if include/drm/drm_ioctl.h exists
[ -f "$inc_drm/drm_ioctl.h" ] && en_flag "_QBP_INCLUDE_HAS_DRM_IOCTL"

## _QBP_INCLUDE_HAS_DRM_PRIME ##
#defined if include/drm/drm_prime.h exists
[ -f "$inc_drm/drm_prime.h" ] && en_flag "_QBP_INCLUDE_HAS_DRM_PRIME"

## _QBP_INCLUDE_HAS_DRM_PRINT ##
#defined if include/drm/drm_print.h exists
[ -f "$inc_drm/drm_print.h" ] && en_flag "_QBP_INCLUDE_HAS_DRM_PRINT"

## _QBP_INCLUDE_SCHED_MM ##
#defined if include/linux/sched/mm.h exists
[ -f "$inc_linux/sched/mm.h" ] && en_flag "_QBP_INCLUDE_SCHED_MM"

## _QBP_INCLUDE_SCHED_SIGNAL ##
#defined if sched/signal.h does NOT exist
[ ! -f "$inc_linux/sched/signal.h" ] && en_flag "_QBP_INCLUDE_SCHED_SIGNAL"

## _QBP_NEED_ACCEL_FOP_MMAP ##
#defined if in include/drm/drm_accel.h, 'define DRM_ACCEL_FOPS' is NOT followed by '.mmap = drm_gem_mmap'
FILE="$inc_drm/drm_accel.h"
ENABLE="none"
if [ -f "$FILE" ] ; then
	#grab all the lines following "define DRM_ACCEL_FOPS" until there's a blank line
	ACCEL_FOPS=$(sed -n "/^#define DRM_ACCEL_FOPS/,/^$/p" "$FILE")
	ENABLE=$(echo "$ACCEL_FOPS" | grep "\.mmap\s\+=")
fi
#if the file doesn't exist Accel framework needs backporting, and thus doesn't need this patch
[ "$ENABLE" != "none" ] && [ -z "$ENABLE" ] && en_flag "_QBP_NEED_ACCEL_FOP_MMAP"

## _QBP_NEED_ATTR_FALLTHROUGH ##
#define if include/linux/compiler_attributes.h does NOT 'define fallthrough'
ENABLE=$(chk_file "define fallthrough" "$inc_linux/compiler_attributes.h")
[ -z "$ENABLE" ] && en_flag "_QBP_NEED_ATTR_FALLTHROUGH"

## _QBP_NEED_BIT_FIELD_PREPGET ##
#TODO: might want to split into 2
#defined if include/linux/build_bug.h does NOT contain BUILD_BUG_ON_MSG, BUILD_BUG_ON_NOT_POWER_OF_2
#defined if include/linux/bitfield.h does NOT contain __bf_shf, FIELD_PREP FIELD_GET
ENA=$(chk_file "define BUILD_BUG_ON_MSG(" "$inc_linux/build_bug.h")
BLE=$(chk_file "define BUILD_BUG_ON_NOT_POWER_OF_2(" "$inc_linux/build_bug.h")
EN=$(chk_file "define __bf_shf(" "$inc_linux/bitfield.h")
AB=$(chk_file "define FIELD_PREP(" "$inc_linux/bitfield.h")
LE=$(chk_file "define FIELD_GET(" "$inc_linux/bitfield.h")
[ -z "$ENA" ] && [ -z "$BLE" ] && [ -z "$EN" ] && [ -z "$AB" ] && [ -z "$LE" ] && en_flag "_QBP_NEED_BIT_FIELD_PREPGET"

## _QBP_NEED_DMA_SYNC_SGTABLE ##
#defined if dma_*_sgtable funcs do NOT exist
find_funcs=( )
find_funcs+=( "dma_map_sgtable(" "dma_sync_sgtable_for_cpu(" "dma_unmap_sgtable(" "dma_sync_sgtable_for_device(" "for_each_sgtable_sg(" )
num_found=0
file_locations=( "$inc_linux/dma-mapping.h" "$inc_linux/scatterlist.h" )
search_locations=( )
for loc in "${file_locations[@]}" ; do
	if [ -f "$loc" ] ; then
		search_locations+=( "$loc" )
	fi
done
if [ ${#search_locations[@]} != 0 ] ; then
	for fun in "${find_funcs[@]}" ; do
		if [ "$( cat "${search_locations[@]}" | grep -c "$fun" )" != 0 ] ; then
			((num_found++))
		fi
	done
fi
if [ $num_found != ${#find_funcs[@]} ] ; then
	en_flag "_QBP_NEED_DMA_SYNC_SGTABLE"
fi

## _QBP_NEED_DRM_ACCEL_FRAMEWORK ##
#defined if 'drm_accel.h' does NOT exist OR if DRM Accel Framework is NOT configured
( ! grep -q "CONFIG_DRM_ACCEL=[ym]" < "$config_file" || [ ! -f "$inc_drm/drm_accel.h" ] ) && en_flag "_QBP_NEED_DRM_ACCEL_FRAMEWORK"

## _QBP_NEED_DRM_DEV_ATOMIC_OPEN_COUNT ##
#pre 4.14 - check include/drm/drmP.h
#post 4.14 - check include/drm/drm_device.h
#defined if struct "drm_device" does NOT have "atomic_t open_count"
#maybe change name of FLAG
ENABLE=$(chk_few_files "$inc_drm/drmP.h" "$inc_drm/drm_device.h" "-" "chk_struct" "atomic_t open_count;" "drm_device")
[ -n "$ENABLE" ] && en_flag "_QBP_NEED_DRM_DEV_ATOMIC_OPEN_COUNT"

## _QBP_NEED_DRM_MANAGED_MUTEX ##
#defined if include/drm/drm_managed.h does NOT have drmm_mutex_init
#but also that NOT QAIC_DRM_MANAGED
ENABLE=$(chk_file "drmm_mutex_init" "$inc_drm/drm_managed.h")
[ -z "$ENABLE" ] && ! [[ ${BACKPORT_FLAGS[*]} =~ "-D_QBP_ALT_DRM_MANAGED" ]] && en_flag "_QBP_NEED_DRM_MANAGED_MUTEX"

## _QBP_NEED_EPOLL ##
#defined if include/uapi/linux/eventpoll.h does NOT contain 'EPOLLIN' 'EPOLLRDHUP'
ENA=$(chk_file "EPOLLIN" "$inc_uapi/eventpoll.h")
BLE=$(chk_file "EPOLLRDHUP" "$inc_uapi/eventpoll.h")
[ -z "$ENA" ] && [ -z "$BLE" ] && en_flag "_QBP_NEED_EPOLL"

## _QBP_NEED_FSLEEP ##
#defined if include/linux/delay.h does NOT include 'void fsleep'
ENABLE=$(chk_file "void fsleep(" "$inc_linux/delay.h")
[ -z "$ENABLE" ] && en_flag "_QBP_NEED_FSLEEP"

## _QBP_NEED_GFP_RETRY_MAYFAIL ##
#pre-6.0 check include/linux/gfp.h
#post-6.0 check include/linux/gfp_types.h
#defined if 'define __GFP_RETRY_MAYFAIL' NOT found
ENABLE=$(chk_few_files "$inc_linux/gfp.h" "$inc_linux/gfp_types.h" "-" "chk_file" "define __GFP_RETRY_MAYFAIL")
[ -n "$ENABLE" ] && en_flag "_QBP_NEED_GFP_RETRY_MAYFAIL"

## _QBP_NEED_HWMON_TEMP_ALARM ##
#defined if include/linux/hwmon.h if 'define HWMON_T_ALARM'
ENABLE=$(chk_file "define HWMON_T_ALARM" "$inc_linux/hwmon.h")
[ -n "$ENABLE" ] && en_flag "_QBP_NEED_HWMON_TEMP_ALARM"

## _QBP_NEED_IDR_INIT_BASE ##
#defined if include/linux/idr.h does NOT contain 'void idr_init_base('
ENABLE=$(chk_file "typvoid idr_init_base(" "$inc_linux/idr.h")
[ -z "$ENABLE" ] && en_flag "_QBP_NEED_IDR_INIT_BASE"

## _QBP_NEED_IRQ_WAKE_THREAD ##
#defined if include/linux/interrupt.h if 'void irq_wake_thread(' does NOT exist
ENABLE=$(chk_file "void irq_wake_thread(" "$inc_linux/interrupt.h")
[ -z "$ENABLE" ] && en_flag "_QBP_NEED_IRQ_WAKE_THREAD"

## _QBP_NEED_LIST_IS_FIRST ##
#defined if include/linux/list.h does NOT include 'int list_is_first('
ENABLE=$(chk_file "int list_is_first(" "$inc_linux/list.h")
[ -z "$ENABLE" ] && en_flag "_QBP_NEED_LIST_IS_FIRST"

## _QBP_NEED_MHI_MODALIAS_DEV_ID ##
#defined if include/linux/mod_devicetable.h does NOT have 'struct mhi_device_id' and 'define MHI_DEVICE_MODALIAS_FMT'
ENA=$(chk_file "struct mhi_device_id" "$inc_linux/mod_devicetable.h")
BLE=$(chk_file "define MHI_DEVICE_MODALIAS_FMT" "$inc_linux/mod_devicetable.h")
[ -z "$ENA" ] && [ -z "$BLE" ] && en_flag "_QBP_NEED_MHI_MODALIAS_DEV_ID"

## _QBP_NEED_OVERFLOW_SIZE_ADD ##
#defined if include/linux/overflow.h does NOT have '__must_check size_add('
ENABLE=$(chk_file "__must_check size_add(" "$inc_linux/overflow.h")
[ -z "$ENABLE" ] && en_flag "_QBP_NEED_OVERFLOW_SIZE_ADD"

## _QBP_NEED_PCI_IRQ_VEC ##
#define if include/linux/pci.h does NOT contain 'int pci_alloc_irq_vectors', 'void pci_free_irq_vectors', 'int pci_irq_vector'
EN=$(chk_file "int pci_alloc_irq_vectors(" "$inc_linux/pci.h")
AB=$(chk_file "void pci_free_irq_vectors(" "$inc_linux/pci.h")
LE=$(chk_file "int pci_irq_vector" "$inc_linux/pci.h")
[ -z "$EN" ] && [ -z "$AB" ] && [ -z "$LE" ] && en_flag "_QBP_NEED_PCI_IRQ_VEC"

## _QBP_NEED_PCI_VENDOR_PHYS_ADDR_MAX ##
#pre 5.1 - include/linux/kernel.h
#post 5.1 - include/linux/limits.h
#defined if 'define PHYS_ADDR_MAX' does NOT exist
#TODO:
#check include/linux/pci_ids.h for 'PCI_VENDOR_ID_QCOM' probably split to diff flags
ENABLE=$(chk_few_files "$inc_linux/kernel.h" "$inc_linux/limits.h" "-" "chk_file" "define PHYS_ADDR_MAX")
[ -n "$ENABLE" ] && en_flag "_QBP_NEED_PCI_VENDOR_PHYS_ADDR_MAX"

## _QBP_NEED_POLL_T ##
#defined if include/uapi/linux/types.h does NOT contain 'typedef unsigned __bitwise __poll_t'
ENABLE=$(chk_file "typedef unsigned __bitwise __poll_t" "$inc_uapi/types.h")
[ -z "$ENABLE" ] && en_flag "_QBP_NEED_POLL_T"

## _QBP_NEED_SENSOR_DEV_ATTR ##
#defined if 'SENSOR_DEVICE_ATTR_RO/RW/WO' do NOT exist
EN=$(chk_file "SENSOR_DEVICE_ATTR_RO" "$inc_linux/hwmon-sysfs.h")
AB=$(chk_file "SENSOR_DEVICE_ATTR_RW" "$inc_linux/hwmon-sysfs.h")
LE=$(chk_file "SENSOR_DEVICE_ATTR_WO" "$inc_linux/hwmon-sysfs.h")
[ -z "$EN" ] && [ -z "$AB" ] && [ -z "$LE" ] && en_flag "_QBP_NEED_SENSOR_DEV_ATTR"

## _QBP_NEED_SYSFS_EMIT ##
#defined if include/linux/sysfs.h does NOT contain "sysfs_emit" "sysfs_emit_at"
ENA=$(chk_file "int sysfs_emit(" "$inc_linux/sysfs.h")
BLE=$(chk_file "int sysfs_emit_at(" "$inc_linux/sysfs.h")
[ -z "$ENA" ] && [ -z "$BLE" ] && en_flag "_QBP_NEED_SYSFS_EMIT"

## _QBP_NEED_TIMER_SETUP ##
#defined if include/linux/timer.h does NOT contain 'timer_setup('
ENABLE=$(chk_file "void timer_setup(" "$inc_linux/timer.h")
DISABLE=$(chk_file "define timer_setup(" "$inc_linux/timer.h")
[ -z "$ENABLE" ] && [ -z "$DISABLE" ] && en_flag "_QBP_NEED_TIMER_SETUP"

## _QBP_NEED_VCALLOC ##
#defined if include/linux/vmalloc.h does NOT include 'void *vcalloc('
ENABLE=$(chk_file "void *vcalloc(" "$inc_linux/vmalloc.h")
[ -z "$ENABLE" ] && en_flag "_QBP_NEED_VCALLOC"

## _QBP_REDEF_DEV_GROUP_ADD_REMOVE ##
#defined if include/linux/device.h does NOT contain 'device_add_group(' 'device_remove_group('
ENA=$(chk_file "device_add_group(" "$inc_linux/device.h")
BLE=$(chk_file "void device_remove_group" "$inc_linux/device.h")
[ -z "$ENA" ] && [ -z "$BLE" ] && en_flag "_QBP_REDEF_DEV_GROUP_ADD_REMOVE"

## _QBP_REDEF_DEVM_DRM_ALLOC ##
#defined if 'devm_drm_dev_alloc' is NOT defined
ENABLE=$(chk_few_files "$inc_drm/drmP.h" "$inc_drm/drm_drv.h" "-" "chk_file" "define devm_drm_dev_alloc(")
[ -n "$ENABLE" ] && en_flag "_QBP_REDEF_DEVM_DRM_ALLOC"

## _QBP_REDEF_DEVM_FREE_PAGE ##
#defined if include/linux/device.h does NOT contain 'unsigned long devm_get_free_pages(' and 'void devm_free_pages('
ENA=$(chk_file "unsigned long devm_get_free_pages(" "$inc_linux/device.h")
BLE=$(chk_file "void devm_free_pages(" "$inc_linux/device.h")
[ -z "$ENA" ] && [ -z "$BLE" ] && en_flag "_QBP_REDEF_DEVM_FREE_PAGE"

## _QBP_REDEF_DRM_DEV_GET_PUT ##
#check include/drm/drm_drv.h if 'void drm_dev_get(' 'void drm_dev_put('
#defined if they do NOT exist
ENA=$(chk_file "void drm_dev_get(" "$inc_drm/drm_drv.h")
BLE=$(chk_file "void drm_dev_put(" "$inc_drm/drm_drv.h")
[ -z "$ENA" ] && [ -z "$BLE" ] && en_flag "_QBP_REDEF_DRM_DEV_GET_PUT"

## _QBP_REDEF_DRM_DEV_UNPLUG ##
#defined if 'drm_device_is_unplugged' is defined / drm_dev_is_unplugged is not
ENABLE=$(chk_file "int drm_device_is_unplugged" "$inc_drm/drmP.h")
[ -n "$ENABLE" ] && en_flag "_QBP_REDEF_DRM_DEV_UNPLUG"

## _QBP_REDEF_DRM_PRINT_INDENT ##
#defined if include/drm/drm_print.h does NOT include 'define drm_printf_indent('
ENABLE=$(chk_file "define drm_printf_indent(" "$inc_drm/drm_drv.h")
[ -z "$ENABLE" ] && en_flag "_QBP_REDEF_DRM_PRINT_INDENT"

## _QBP_REDEF_GEM_OBJ_GETPUT_OLDER ##
#defined if include/drm/drm_gem.h does NOT contain "void drm_gem_object_get("
#this also enables _QBP_REDEF_GEM_OBJ_PUT since its use nested.
ENABLE=$(chk_file "void drm_gem_object_get(" "$inc_drm/drm_gem.h")
[ -z "$ENABLE" ] && en_flag "_QBP_REDEF_GEM_OBJ_GETPUT_OLDER" && en_flag "_QBP_REDEF_GEM_OBJ_PUT"

## _QBP_REDEF_GEM_OBJ_PUT ##
#check if 'drm_gem_object_put_unlocked' is defined
#this flag is also enabled elsewhere. see _QBP_REDEF_GEM_OBJ_GETPUT_OLDER
ENABLE=$(chk_file "void drm_gem_object_put_unlocked(" "$inc_drm/drm_gem.h")
[ -n "$ENABLE" ] && en_flag "_QBP_REDEF_GEM_OBJ_PUT"
#first switch happens when drm_gem_object_put_unlocked is defined
#second switch happens when drm_gem_object_get is no longer defined

## _QBP_REDEF_IDA_FREE ##
#check include/linux/idr.h for 'void ida_free('
ENABLE=$(chk_file "void ida_free(" "$inc_linux/idr.h")
[ -z "$ENABLE" ] && en_flag "_QBP_REDEF_IDA_FREE"

## _QBP_REDEF_KVFREE ##
#defined if include/linux/mm.h /slab.h doesn't have 'void kvfree('
ENABLE=$(chk_few_files "$inc_linux/mm.h" "$inc_linux/slab.h" "-" "chk_file" "void kvfree(")
[ -n "$ENABLE" ] && en_flag "_QBP_REDEF_KVFREE"

## _QBP_REDEF_KVMALLOC_ARRAY ##
#pre-5.16 include/linux/mm.h
#post-5.16 include/linux/slab.h
#defined if 'void *kvmalloc_array(' NOT found
ENABLE=$(chk_few_files "$inc_linux/mm.h" "$inc_linux/slab.h" "-" "chk_file" "void *kvmalloc_array(")
[ -n "$ENABLE" ] && en_flag "_QBP_REDEF_KVMALLOC_ARRAY"

## _QBP_REDEF_PCI_PRINTK ##
#check include/linux/pci.h if 'define pci_dbg/err/warn/info,printk'
ENABLE=$(chk_file "define pci_printk(" "$inc_linux/pci.h")
[ -z "$ENABLE" ] && en_flag "_QBP_REDEF_PCI_PRINTK"

#export the final backport flags
#file is cleared by DKMS after successful build (and all files in build dir are
#removed before new builds)
echo "${BACKPORT_FLAGS[@]}" > qaic_backport_flags

