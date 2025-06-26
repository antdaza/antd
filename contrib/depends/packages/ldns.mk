package=ldns
$(package)_version=1.8.4
$(package)_download_path=https://www.nlnetlabs.nl/downloads/ldns/
$(package)_file_name=$(package)-$($(package)_version).tar.gz
$(package)_sha256_hash=838b907594baaff1cd767e95466a7745998ae64bc74be038dccc62e2de2e4247
$(package)_dependencies=openssl

define $(package)_set_vars
  $(package)_config_opts=--disable-shared --enable-static --disable-dane-ta-usage --with-drill 
  $(package)_config_opts=--with-ssl=$(host_prefix) 
  $(package)_config_opts_release=--disable-debug-mode
  $(package)_config_opts_linux=--with-pic
endef

define $(package)_patch_cmds
  patch -p1 < $($(package)_patch_dir)/ldns-openssl3-fix.patch
endef

define $(package)_preprocess_cmds
   cp -f $(BASEDIR)/config.guess $(BASEDIR)/config.sub .
endef

define $(package)_config_cmds
  $($(package)_autoconf)
endef

define $(package)_build_cmds
  $(MAKE)
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install-h install-lib
endef

define $(package)_postprocess_cmds
endef
