obj-$(CONFIG_HAGENT) += hagent.o hagent_balloon.o

RUSTFLAGS_lib.o += --extern hashbrown --extern lazy_static

hagent-objs += helper.o migrate.o param.o hook.o lib.o

hagent_balloon-objs += balloon.o

obj-$(CONFIG_HAGENT) += placement/
