BUILD_DIR = build
LABWC_DIR = subprojects/labwc
LABWC_BUILD = $(LABWC_DIR)/build

all: compile

$(BUILD_DIR)/build.ninja:
	meson setup $(BUILD_DIR) || { rm -rf $(BUILD_DIR); meson setup $(BUILD_DIR); }

$(LABWC_BUILD)/build.ninja:
	meson setup $(LABWC_BUILD) $(LABWC_DIR) --prefix=/usr --buildtype=release || { rm -rf $(LABWC_BUILD); meson setup $(LABWC_BUILD) $(LABWC_DIR) --prefix=/usr --buildtype=release; }

labwc: $(LABWC_BUILD)/build.ninja
	meson compile -C $(LABWC_BUILD)

compile: $(BUILD_DIR)/build.ninja labwc
	meson compile -C $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) $(LABWC_BUILD)

install: install-opt

run: compile
	mkdir -p $(BUILD_DIR)/share/applications
	mkdir -p $(BUILD_DIR)/share/icons/hicolor/scalable/apps
	cp data/*.desktop $(BUILD_DIR)/share/applications/
	cp -r data/icons/* $(BUILD_DIR)/share/icons/
	gtk-update-icon-cache -f -t $(BUILD_DIR)/share/icons/hicolor

reconfigure:
	meson setup $(BUILD_DIR) --reconfigure
	meson setup $(LABWC_BUILD) $(LABWC_DIR) --reconfigure

schemas:
	glib-compile-schemas data/

install-session:
	@if [ -n "$$container" ]; then \
		host-spawn run0 bash $(CURDIR)/subprojects/singularity-session/scripts/install-session.sh; \
		host-spawn run0 bash $(CURDIR)/subprojects/singularity-session/scripts/install-gdm-config.sh; \
	else \
		run0 bash $(CURDIR)/subprojects/singularity-session/scripts/install-session.sh; \
		run0 bash $(CURDIR)/subprojects/singularity-session/scripts/install-gdm-config.sh; \
	fi

deploy-host: compile
	bash scripts/deploy-to-host.sh

# Install to /opt/local — the persistent writable prefix on Vanilla OS.
# Requires elevated privileges; uses host-spawn run0 when running inside a container.
install-opt: compile
	@if [ -n "$$container" ]; then \
		echo "Inside container: bundling libraries..."; \
		mkdir -p $(BUILD_DIR)/extra-libs; \
		find /usr/lib /usr/local/lib -name "libwlroots*.so*" -exec cp -a {} $(BUILD_DIR)/extra-libs/ \; 2>/dev/null || true; \
		find /usr/lib /usr/local/lib -name "libsfdo*.so*" -exec cp -a {} $(BUILD_DIR)/extra-libs/ \; 2>/dev/null || true; \
		find /usr/lib /usr/local/lib -name "libgtk4-layer-shell*.so*" -exec cp -a {} $(BUILD_DIR)/extra-libs/ \; 2>/dev/null || true; \
		find /usr/lib /usr/local/lib -name "libpeas-2*.so*" -exec cp -a {} $(BUILD_DIR)/extra-libs/ \; 2>/dev/null || true; \
		echo "Bundling complete. Elevating to host for installation..."; \
		host-spawn run0 bash $(CURDIR)/scripts/install-to-opt.sh; \
	else \
		run0 bash $(CURDIR)/scripts/install-to-opt.sh; \
	fi

.PHONY: all compile labwc clean install run reconfigure schemas deploy-host install-session install-opt
