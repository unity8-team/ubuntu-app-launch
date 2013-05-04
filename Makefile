default: desktop-exec
	@echo "Building"

desktop-exec: desktop-exec.c
	gcc -o desktop-exec desktop-exec.c `pkg-config --cflags --libs gio-unix-2.0`

install: application.conf desktop-exec
	mkdir -p $(DESTDIR)/usr/share/upstart/sessions
	install -m 644 application.conf $(DESTDIR)/usr/share/upstart/sessions/
	mkdir -p /usr/lib/$(DEB_BUILD_MULTIARCH)/upstart-app-launch/

