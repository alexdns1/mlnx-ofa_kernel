AC_PREREQ([2.57])
AC_INIT([compat_mlnx], 2.3, [http://support.mellanox.com/SupportWeb/service_center/SelfService], [compat_mlnx])

AC_CONFIG_AUX_DIR([config])
AC_CONFIG_MACRO_DIR([config])

AM_INIT_AUTOMAKE([foreign])
AC_CONFIG_FILES([Makefile])

AC_ARG_WITH(njobs,
	AS_HELP_STRING([--with-njobs=N],[Allow N jobs at once; jobs as number of CPUs with no arg.]),
	[
		NJOBS="$withval"
		case "X${NJOBS}" in
			X | X[A-Za-z]*)
			NJOBS=$(MLXNUMC=$(grep ^processor /proc/cpuinfo | wc -l) && echo $(($MLXNUMC<16?$MLXNUMC:16)))
			;;
		esac
	],
	NJOBS=1
)

MLNX_PROG_LINUX

AC_OUTPUT

#
#       cleanup auto-generated defines
#
sed -i '/\<PACKAGE\>/d' $PWD/config.h
sed -i '/\<PACKAGE_BUGREPORT\>/d' $PWD/config.h
sed -i '/\<PACKAGE_NAME\>/d' $PWD/config.h
sed -i '/\<PACKAGE_STRING\>/d' $PWD/config.h
sed -i '/\<PACKAGE_TARNAME\>/d' $PWD/config.h
sed -i '/\<PACKAGE_URL\>/d' $PWD/config.h
sed -i '/\<PACKAGE_VERSION\>/d' $PWD/config.h
sed -i '/\<VERSION\>/d' $PWD/config.h

#
cat << 'END_CONFIG' >> $PWD/config.h

/* Make sure LINUX_BACKPORT macro is defined for all external users */
#ifndef LINUX_BACKPORT
#define LINUX_BACKPORT(__sym) backport_ ##__sym
#endif

END_CONFIG
