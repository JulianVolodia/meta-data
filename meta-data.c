/*
 * Copyright (c) 2017 Reyk Floeter <reyk@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include <net/if.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>
#include <net/if_bridge.h>
#include <arpa/inet.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <errno.h>
#include <err.h>

#include <kcgi.h>
#include <kcgihtml.h>

#define LEASE_FILE	"/var/db/dhcpd.leases"
#define BRIDGE_NAME	"bridge0"
#define DATA_USER	"_meta-data"

struct lease {
	struct ether_addr	 l_lladdr;
	struct in_addr		 l_ipaddr;
	TAILQ_ENTRY(lease)	 l_entry;
};
TAILQ_HEAD(leases, lease) leases;

struct vm {
	struct lease		*vm_lease;
	char			 vm_ifname[IFNAMSIZ];
	char			 vm_ifdescr[IFDESCRSIZE];
	char			*vm_instance_id;
	char			*vm_interface_name;
	char			*vm_local_hostname;
};

enum pageids {
	PAGE_INDEX,
	PAGE_META_DATA,
	PAGE_USER_DATA,
	PAGE__MAX
};

const char *pagenames[PAGE__MAX] = {
	".",
	"meta-data",
	"user-data"
};

void	page_home(struct kreq *, struct vm *);
void	page_meta_data(struct kreq *, struct vm *);
void	page_user_data(struct kreq *, struct vm *);
void	page_error(struct kreq *, int);

struct page {
	enum pageids	 page_id;
	void		(*page_cb)(struct kreq *, struct vm *);
} pages[] = {
	{ PAGE_INDEX,		page_home },
	{ PAGE_META_DATA,	page_meta_data },
	{ PAGE_USER_DATA,	page_user_data },
	{ PAGE__MAX,		page_home },
};

char *
parse_value(const char *s1, char *s2)
{
	size_t	 len = strlen(s1);
	char	*v;

	if (strncmp(s1, s2, len) != 0)
		return (NULL);

	v = s2 + len;
	v[strcspn(v, " \t\r\n;{}\0")] = '\0';

	return (v);
}

void
parse_leases(FILE *fp)
{
	char		 buf[BUFSIZ], *k, *v;
	struct lease	*l = NULL;

	TAILQ_INIT(&leases);

	if (fseek(fp, 0, SEEK_SET) == -1)
		err(1, "can't rewind lease file");

	while (fgets(buf, sizeof(buf), fp) != NULL) {
		k = buf + strspn(buf, " \t");
		
		if ((v = parse_value("lease ", k)) != NULL) {
			if ((l = calloc(1, sizeof(*l))) == NULL)
				err(1, "cannot add lease");

			inet_pton(AF_INET, v, &l->l_ipaddr);

			/* insert in reverse order */
			TAILQ_INSERT_HEAD(&leases, l, l_entry);
		} else if ((v =
		    parse_value("hardware ethernet ", k)) != NULL) {
			if (l == NULL)
				err(1, "syntax error");
			l->l_lladdr = *ether_aton(v);
		}
	}
}

void
free_leases(void)
{
	struct lease	*l, *next;

	TAILQ_FOREACH_SAFE(l, &leases, l_entry, next) {
		TAILQ_REMOVE(&leases, l, l_entry);
		free(l);
	}
}

struct lease *
find_lease(const char *addr)
{
	struct lease	*l;
	struct in_addr	 ipaddr;

	memset(&ipaddr, 0, sizeof(ipaddr));
	inet_pton(AF_INET, addr, &ipaddr);

	TAILQ_FOREACH(l, &leases, l_entry) {
		if (ipaddr.s_addr == l->l_ipaddr.s_addr)
			return (l);
	}

	return (NULL);
}

int
find_vm(int s, const char *name, struct vm *vm)
{
	struct ifbaconf	 ifbac;
	struct ifbareq	*ifba;
	struct ifreq	 ifr;
	char		*inbuf = NULL, *p;
	struct lease	*l = vm->vm_lease;
	int		 ret = -1;
	size_t		 i, len = BUFSIZ;

	while (1) {
		ifbac.ifbac_len = len;
		if ((p = realloc(inbuf, len)) == NULL)
			err(1, "malloc");
		ifbac.ifbac_buf = inbuf = p;
		strlcpy(ifbac.ifbac_name, name, sizeof(ifbac.ifbac_name));
		if (ioctl(s, SIOCBRDGRTS, &ifbac) < 0) {
			if (errno == ENETDOWN)
				return (-1);
			err(1, "%s", name);
		}
		if (ifbac.ifbac_len + sizeof(*ifba) < len)
			break;
		len *= 2;
	}

	for (i = 0; i < ifbac.ifbac_len / sizeof(*ifba); i++) {
		ifba = ifbac.ifbac_req + i;

		if (memcmp(&ifba->ifba_dst, &l->l_lladdr,
		    sizeof(l->l_lladdr)) == 0) {
			strlcpy(vm->vm_ifname, ifba->ifba_ifsname,
			    sizeof(vm->vm_ifname));
			ret = 0;
			break;
		}
	}
	free(inbuf);

	if (ret == 0) {
		memset(&ifr, 0, sizeof(ifr));
		strlcpy(ifr.ifr_name, vm->vm_ifname, sizeof(ifr.ifr_name));
		ifr.ifr_data = (caddr_t)&vm->vm_ifdescr;

		if (ioctl(s, SIOCGIFDESCR, &ifr) == 0 &&
		    strlen(ifr.ifr_data)) {
			vm->vm_instance_id = p = vm->vm_ifdescr;
			if ((p = strchr(p, '-')) != NULL) {
				*p++ = '\0';
				vm->vm_interface_name = p;
			}
			if (p != NULL &&
			    (p = strchr(p, '-')) != NULL) {
				*p++ = '\0';
				vm->vm_local_hostname = p;
			}
		}
		if (vm->vm_local_hostname == NULL)
			ret = -1;
	}

	return (ret);
}

void
html_entry(struct khtmlreq *req, const char *name)
{
	khtml_attr(req, KELEM_A, KATTR_HREF, name, KATTR__MAX);
	khtml_puts(req, name);
	khtml_closeelem(req, 2);
	khtml_elem(req, KELEM_BR);
	khtml_puts(req, "\n");
	khtml_closeelem(req, 1);
}

void
page_index(struct kreq *r, const char *names[], size_t namesz)
{
	struct khtmlreq	 req;
	size_t		 i;

	khttp_head(r, kresps[KRESP_STATUS], "%s", khttps[KHTTP_200]);
	khttp_body(r);

	khtml_open(&req, r, KHTML_PRETTY);

	for (i = 0; i < namesz; i++)
		html_entry(&req, names[i]);

	khtml_close(&req);
}

void
page_home(struct kreq *r, struct vm *vm)
{
	page_index(r, pagenames, PAGE__MAX);
}

void
page_file_data(struct kreq *r, struct vm *vm, const char *name)
{
	char		 path[PATH_MAX], buf[BUFSIZ];
	FILE		*fp = NULL;
	size_t		 len;

	snprintf(path, sizeof(path), "%s/%s",
	    vm->vm_local_hostname, name);
	if ((fp = fopen(path, "r")) == NULL) {
		page_error(r, KHTTP_404);
		return;
	}

	khttp_head(r, kresps[KRESP_STATUS], "%s", khttps[KHTTP_200]);
	khttp_body(r);
	do {
		if ((len = fread(buf, 1, sizeof(buf), fp)) == 0)
			break;
		khttp_write(r, buf, len);
	} while (len == sizeof(buf));
	fclose(fp);
}

void
page_meta_data(struct kreq *r, struct vm *vm)
{
	const char	*str = NULL;
	const char	*names[] = {
		"local-hostname",
		"instance-id",
		"public-keys/0/openssh-key"
	};
	size_t		 namesz = 3;

	if (strcmp(names[0], r->path) == 0)
		str = vm->vm_local_hostname;
	else if (strcmp(names[1], r->path) == 0)
		str = vm->vm_instance_id;
	else if (strcmp(names[2], r->path) == 0) {
		page_file_data(r, vm, "openssh-key");
		return;
	} else if (*r->path == '\0') {
		page_index(r, names, namesz);
		return;
	}

	if (str == NULL) {
		page_error(r, KHTTP_404);
		return;
	}

	khttp_head(r, kresps[KRESP_STATUS], "%s", khttps[KHTTP_200]);
	khttp_body(r);
	khttp_puts(r, str);
}

void
page_user_data(struct kreq *r, struct vm *vm)
{
	page_file_data(r, vm, "user-data");
}

void
page_error(struct kreq *r, int code)
{
	khttp_head(r, kresps[KRESP_STATUS], "%s", khttps[code]);
	khttp_body(r);
	khttp_puts(r, khttps[code]);
}

int
main(int argc, char *argv[])
{
	struct lease	*l;
	struct kreq	 r;
	struct kfcgi	*fcgi;
	struct page	*p = NULL;
	struct vm	 vm;
	size_t		 i;
	FILE		*fp;
	int		 s;
	void		(*cb)(struct kreq *, struct vm *);
	struct passwd	*pw;
	const char	*bridge = BRIDGE_NAME;

	if ((fp = fopen(LEASE_FILE, "r")) == NULL)
		err(1, "can't open lease file");

	if ((pw = getpwnam(DATA_USER)) == NULL)
		err(1, "can't get user");

	if (chroot(pw->pw_dir) == -1)
		err(1, "chroot");
	if (chdir("/") == -1)
		err(1, "chdir(\"/\")");

	if (setgroups(1, &pw->pw_gid) ||
	    setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid) ||
	    setresuid(pw->pw_uid, pw->pw_uid, pw->pw_uid))
		err(1, "cannot drop privileges");

	if (argc > 1)
		bridge = argv[1];

	if ((s = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
		err(1, "can't open ioctl socket");

	if (khttp_fcgi_init(&fcgi, NULL, 0,
	    pagenames, PAGE__MAX, 0) != KCGI_OK)
		errx(1, "khttp_fcgi_init");

	while (khttp_fcgi_parse(fcgi, &r) == KCGI_OK) {
		parse_leases(fp);

		cb = NULL;
		l = find_lease(r.remote);

		for (i = 0; l != NULL && i < PAGE__MAX; i++) {
			p = &pages[i];
			if (p->page_id == r.page) {
				cb = p->page_cb;
				break;
			}
		}

		if (l == NULL)
			page_error(&r, KHTTP_401);
		else if (cb == NULL)
			page_error(&r, KHTTP_404);
		else {
			memset(&vm, 0, sizeof(vm));
			vm.vm_lease = l;
			if (find_vm(s, bridge, &vm) == -1)
				page_error(&r, KHTTP_404);
			else
				(*cb)(&r, &vm);
		}

		khttp_free(&r);
		free_leases();
	}

	khttp_fcgi_free(fcgi);
	fclose(fp);
	close(s);

	return (EXIT_SUCCESS);
}