/*
 * click-install.cc -- configuration installer for Click kernel module
 * Eddie Kohler
 *
 * Copyright (c) 1999-2000 Massachusetts Institute of Technology
 * Copyright (c) 2000 Mazu Networks, Inc.
 * Copyright (c) 2002 International Computer Science Institute
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Click LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Click LICENSE file; the license in that file is
 * legally binding.
 */

#include <click/config.h>

#include "common.hh"
#include "routert.hh"
#include "lexert.hh"
#include <click/error.hh>
#include <click/confparse.hh>
#include <click/clp.h>
#include <click/driver.hh>
#include "toolutils.hh"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cerrno>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#if FOR_BSDMODULE
# include <sys/param.h>
# include <sys/mount.h>
#elif FOR_LINUXMODULE && HAVE_CLICKFS
# include <sys/mount.h>
#endif
#include <fcntl.h>
#include <unistd.h>

#define HELP_OPT		300
#define VERSION_OPT		301
#define CLICKPATH_OPT		302
#define ROUTER_OPT		303
#define EXPRESSION_OPT		304
#define UNINSTALL_OPT		305
#define HOTSWAP_OPT		306
#define MAP_OPT			307
#define VERBOSE_OPT		308
#define THREADS_OPT		309
#define PRIVATE_OPT		310
#define PRIORITY_OPT		311

static Clp_Option options[] = {
  { "cabalistic", 0, PRIVATE_OPT, 0, Clp_Negate },
  { "clickpath", 'C', CLICKPATH_OPT, Clp_ArgString, 0 },
  { "expression", 'e', EXPRESSION_OPT, Clp_ArgString, 0 },
  { "file", 'f', ROUTER_OPT, Clp_ArgString, 0 },
  { "help", 0, HELP_OPT, 0, 0 },
  { "hot-swap", 'h', HOTSWAP_OPT, 0, Clp_Negate },
  { "hotswap", 'h', HOTSWAP_OPT, 0, Clp_Negate },
  { "priority", 'n', PRIORITY_OPT, Clp_ArgInt, 0 },
#if FOR_LINUXMODULE
  { "map", 'm', MAP_OPT, 0, 0 },
  { "private", 'p', PRIVATE_OPT, 0, Clp_Negate },
  { "threads", 't', THREADS_OPT, Clp_ArgUnsigned, 0 },
#endif
  { "uninstall", 'u', UNINSTALL_OPT, 0, Clp_Negate },
  { "verbose", 'V', VERBOSE_OPT, 0, Clp_Negate },
  { "version", 'v', VERSION_OPT, 0, Clp_Negate },
};

static const char *program_name;
#if FOR_LINUXMODULE 
static bool output_map;
#endif

static String::Initializer string_initializer;
static String tmpdir;
static String click_compile_prog;

void
short_usage()
{
  fprintf(stderr, "Usage: %s [OPTION]... [ROUTERFILE]\n\
Try `%s --help' for more information.\n",
	  program_name, program_name);
}

void
usage()
{
  printf("\
`Click-install' installs a kernel Click configuration. It loads the Click\n\
kernel module, and any other necessary modules, as required.\n\
\n\
Usage: %s [OPTION]... [ROUTERFILE]\n\
\n\
Options:\n\
  -f, --file FILE          Read router configuration from FILE.\n\
  -e, --expression EXPR    Use EXPR as router configuration.\n\
  -h, --hot-swap           Hot-swap install new configuration.\n\
  -u, --uninstall          Uninstall Click from kernel, then reinstall.\n\
  -n, --priority N         Set kernel thread priority to N (lower is better).\n", program_name);
#if FOR_LINUXMODULE
  printf("\
  -p, --private            Make /proc/click readable only by root.\n\
  -t, --threads N          Use N threads (multithreaded Click only).\n\
  -m, --map                Print load map to the standard output.\n");
#endif
  printf("\
  -V, --verbose            Print information about files installed.\n\
  -C, --clickpath PATH     Use PATH for CLICKPATH.\n\
      --help               Print this message and exit.\n\
  -v, --version            Print version number and exit.\n\
\n\
Report bugs to <click@pdos.lcs.mit.edu>.\n");
}

static void
prepare_tmpdir(RouterT *r, ErrorHandler *errh)
{
  ContextErrorHandler cerrh(errh, "While preparing to compile packages:");
  BailErrorHandler berrh(&cerrh);
  
  // change to temporary directory
  tmpdir = click_mktmpdir(&berrh);
  assert(tmpdir);
  if (chdir(tmpdir.cc()) < 0)
    berrh.fatal("cannot chdir to %s: %s", tmpdir.cc(), strerror(errno));

  // find compile program
  click_compile_prog = clickpath_find_file("click-compile", "bin", CLICK_BINDIR, &cerrh);
  assert(click_compile_prog);

  // look for .hh files
  if (r) {
    const Vector<ArchiveElement> &archive = r->archive();  
    for (int i = 0; i < archive.size(); i++)
      if (archive[i].name.substring(-3) == ".hh") {
	String filename = archive[i].name;
	FILE *f = fopen(filename.c_str(), "w");
	if (!f)
	  cerrh.warning("%s: %s", filename.c_str(), strerror(errno));
	else {
	  fwrite(archive[i].data.data(), 1, archive[i].data.length(), f);
	  fclose(f);
	}
      }
  }
}

static void
compile_archive_packages(RouterT *r, ErrorHandler *errh)
{
  Vector<String> requirements = r->requirements();

  // go over requirements
  for (int i = 0; i < requirements.size(); i++) {
    const String &req = requirements[i];

    // skip if already have object file
    if (r->archive_index(req + OBJSUFFIX) >= 0)
      continue;

    // look for source file, prepare temporary directory
    int source_ae = r->archive_index(req + CXXSUFFIX);
    if (source_ae < 0)
      source_ae = r->archive_index(req + ".cc");
    if (source_ae < 0)
      continue;
    if (!tmpdir)
      prepare_tmpdir(r, errh);

    // found source file, so compile it
    ArchiveElement ae = r->archive(source_ae);
    errh->message("Compiling package %s from config archive", ae.name.cc());
    ContextErrorHandler cerrh
      (errh, "While compiling package `" + req + OBJSUFFIX "':");

    // write .cc file
    String filename = req + ".cc";
    String source_text = ae.data;
    FILE *f = fopen(filename.c_str(), "w");
    if (!f)
      cerrh.fatal("%s: %s", filename.c_str(), strerror(errno));
    fwrite(source_text.data(), 1, source_text.length(), f);
    fclose(f);
    
    // run click-compile
    String compile_command = click_compile_prog + " --target=" COMPILETARGET " --package=" + req + OBJSUFFIX " " + filename;
    int compile_retval = system(compile_command.cc());
    if (compile_retval == 127)
      cerrh.fatal("could not run `%s'", compile_command.cc());
    else if (compile_retval < 0)
      cerrh.fatal("could not run `%s': %s", compile_command.cc(), strerror(errno));
    else if (compile_retval != 0)
      cerrh.fatal("`%s' failed", compile_command.cc());
    
    // grab object file and add to archive
    ArchiveElement obj_ae = init_archive_element(req + OBJSUFFIX, 0600);
    obj_ae.data = file_string(req + OBJSUFFIX, &cerrh);
    r->add_archive(obj_ae);
  }
}

static void
install_module(const String &filename, const String &options,
	       ErrorHandler *errh)
{
#if FOR_LINUXMODULE
  String cmdline = "/sbin/insmod ";
  if (output_map)
    cmdline += "-m ";
  cmdline += filename;
  if (options)
    cmdline += " " + options;
  int retval = system(cmdline.cc());
  if (retval != 0)
    errh->fatal("`%s' failed", cmdline.cc());
#else
  String cmdline = "/sbin/kldload " + filename;
  assert(!options);
  int retval = system(cmdline.cc());
  if (retval != 0)
    errh->fatal("`%s' failed", cmdline.cc());
#endif
}

static void
install_required_packages(RouterT *r, HashMap<String, int> &packages,
			  HashMap<String, int> &active_modules,
			  ErrorHandler *errh)
{
  // check for uncompiled archive packages and try to compile them
  compile_archive_packages(r, errh);
  
  Vector<String> requirements = r->requirements();

  // go over requirements
  for (int i = 0; i < requirements.size(); i++) {
    String req = requirements[i];

    // look for object in archive
    int obj_aei = r->archive_index(req + OBJSUFFIX);
    if (obj_aei >= 0) {
      // install archived objects. mark them with leading underscores.
      // may require renaming to avoid clashes in `insmod'
      
      // choose module name
      String insmod_name = "_" + req + OBJSUFFIX;
      while (active_modules[insmod_name] >= 0)
	insmod_name = "_" + insmod_name;

      if (verbose)
	errh->message("Installing package %s (%s" OBJSUFFIX " from config archive)", insmod_name.cc(), req.cc());
      
      // install module
      if (!tmpdir)
	prepare_tmpdir(0, errh);
      const ArchiveElement &ae = r->archive(obj_aei);
      String tmpnam = tmpdir + insmod_name;
      FILE *f = fopen(tmpnam.c_str(), "w");
      if (!f)
	errh->fatal("%s: %s", tmpnam.c_str(), strerror(errno));
      fwrite(ae.data.data(), 1, ae.data.length(), f);
      fclose(f);

      install_module(tmpnam, String(), errh);
      
      // cleanup
      packages.insert(req, 1);
      active_modules.insert(insmod_name, 1);
      
    } else if (packages[req] < 0) {
      // install required package from CLICKPATH
      String filename = req + OBJSUFFIX;
      String pathname = clickpath_find_file(filename, "lib", CLICK_LIBDIR);
      if (!pathname) {
	filename = req + ".o";
	pathname = clickpath_find_file(filename, "lib", CLICK_LIBDIR);
	if (!pathname)
	  errh->fatal("cannot find required package `%s" OBJSUFFIX "'\nin CLICKPATH or `%s'", req.cc(), CLICK_LIBDIR);
      }

      // install module
      if (verbose)
	errh->message("Installing package %s (%s)", req.cc(), pathname.cc());

      install_module(pathname, String(), errh);

      packages.insert(req, 1);
      active_modules.insert(filename, 1);
      
    } else {
      // package already loaded; note in 'active_modules' that we still need
      // it
      String filename = req + OBJSUFFIX;
      if (active_modules[filename] < 0)
	filename = req + ".o";
      if (active_modules[filename] == 0)
	active_modules.insert(filename, 1);
    }
  }
}

int
main(int argc, char **argv)
{
  String::static_initialize();
  ErrorHandler::static_initialize(new FileErrorHandler(stderr));
  ErrorHandler *nop_errh = ErrorHandler::default_handler();
  ErrorHandler *errh = new PrefixErrorHandler(nop_errh, "click-install: ");
  CLICK_DEFAULT_PROVIDES;

  // read command line arguments
  Clp_Parser *clp =
    Clp_NewParser(argc, argv, sizeof(options) / sizeof(options[0]), options);
  Clp_SetOptionChar(clp, '+', Clp_ShortNegated);
  program_name = Clp_ProgramName(clp);

  const char *router_file = 0;
  bool file_is_expr = false;
  bool uninstall = false;
  bool hotswap = false;
  int priority = -100;
#if FOR_LINUXMODULE
  bool accessible = true;
  int threads = 1;
  output_map = false;
#endif
  
  while (1) {
    int opt = Clp_Next(clp);
    switch (opt) {
      
     case HELP_OPT:
      usage();
      exit(0);
      break;
      
     case VERSION_OPT:
      printf("click-install (Click) %s\n", CLICK_VERSION);
      printf("Click packages in %s, binaries in %s\n", CLICK_LIBDIR, CLICK_BINDIR);
      printf("Copyright (c) 1999-2000 Massachusetts Institute of Technology\n\
Copyright (c) 2000 Mazu Networks, Inc.\n\
Copyright (c) 2002 International Computer Science Institute\n\
This is free software; see the source for copying conditions.\n\
There is NO warranty, not even for merchantability or fitness for a\n\
particular purpose.\n");
      exit(0);
      break;

     case CLICKPATH_OPT:
      set_clickpath(clp->arg);
      break;
      
     case ROUTER_OPT:
     case EXPRESSION_OPT:
     case Clp_NotOption:
      if (router_file) {
	errh->error("router configuration specified twice");
	goto bad_option;
      }
      router_file = clp->arg;
      file_is_expr = (opt == EXPRESSION_OPT);
      break;

#if FOR_LINUXMODULE
     case THREADS_OPT:
      threads = clp->val.u;
      if (threads < 1) {
        errh->error("must have at least one thread");
	goto bad_option;
      }
      break;

     case PRIVATE_OPT:
      accessible = clp->negated;
      break;

     case PRIORITY_OPT:
      priority = clp->val.i;
      break;

     case MAP_OPT:
      output_map = !clp->negated;
      break;
#endif

     case UNINSTALL_OPT:
      uninstall = !clp->negated;
      break;

     case HOTSWAP_OPT:
      hotswap = !clp->negated;
      break;

     case VERBOSE_OPT:
      verbose = !clp->negated;
      break;
      
     bad_option:
     case Clp_BadOption:
      short_usage();
      exit(1);
      break;
      
     case Clp_Done:
      goto done;
      
    }
  }
  
 done:
  // check options
  if (hotswap && uninstall)
    errh->warning("`--hotswap' and `--uninstall' are mutually exclusive");
  
  RouterT *r = read_router(router_file, file_is_expr, nop_errh);
  if (r)
    r->flatten(nop_errh);
  if (!r || errh->nerrors() > 0)
    exit(1);

  // pathnames of important Click files
  String clickfs_config = clickfs_prefix + String("/config");
  String clickfs_hotconfig = clickfs_prefix + String("/hotconfig");
  String clickfs_errors = clickfs_prefix + String("/errors");
  String clickfs_packages = clickfs_prefix + String("/packages");
  String clickfs_priority = clickfs_prefix + String("/priority");
  
  // uninstall Click if requested
  if (uninstall)
    unload_click(errh);
  
  // install Click module if required
  if (access(clickfs_packages.c_str(), F_OK) < 0) {
#if FOR_LINUXMODULE && HAVE_CLICKFS
    // find and install proclikefs.o
    StringMap modules(-1);
    if (read_active_modules(modules, errh) && modules["proclikefs"] < 0) {
      String proclikefs_o =
	clickpath_find_file("proclikefs.o", "lib", CLICK_LIBDIR, errh);
      if (verbose)
	errh->message("Installing proclikefs (%s)", proclikefs_o.cc());
      install_module(proclikefs_o, String(), errh);
    }
#endif
    
    // find loadable module 
#if FOR_LINUXMODULE
    String click_o =
      clickpath_find_file("click.o", "lib", CLICK_LIBDIR, errh);
#elif FOR_BSDMODULE
    String click_o =
      clickpath_find_file("click.ko", "lib", CLICK_LIBDIR, errh);
#endif
    if (verbose)
      errh->message("Installing Click module (%s)", click_o.cc());

    // install it in the kernel
#if FOR_LINUXMODULE
    String options;
    if (threads > 1)
      options += "threads=" + String(threads);
    if (!accessible)
      options += " accessible=0";
    install_module(click_o, options, errh);
#elif FOR_BSDMODULE
    install_module(click_o, String(), errh);
#endif

#if FOR_BSDMODULE || (FOR_LINUXMODULE && HAVE_CLICKFS)
    // make clickfs_prefix directory if required
    if (access(clickfs_prefix, F_OK) < 0 && errno == ENOENT) {
      if (mkdir(clickfs_prefix, 0777) < 0)
	errh->fatal("cannot make directory %s: %s", clickfs_prefix, strerror(errno));
    }
    
    // mount Click file system
    if (verbose)
      errh->message("Mounting Click module at %s", clickfs_prefix);
# if FOR_BSDMODULE
    int mount_retval = mount("click", clickfs_prefix, 0, 0);
# else
    int mount_retval = mount("none", clickfs_prefix, "click", 0, 0);
# endif
    if (mount_retval < 0 && (verbose || errno != EBUSY))
      errh->error("cannot mount %s: %s", clickfs_prefix, strerror(errno));
#endif

    // check that all is well
    if (access(clickfs_packages.c_str(), F_OK) < 0)
      errh->fatal("cannot install Click module");
  } else {
#if FOR_LINUXMODULE
    if (threads > 1)
      errh->warning("Click module already installed, `--threads' ignored");
#endif
  }

  // find current packages
  HashMap<String, int> active_modules(-1);
  HashMap<String, int> packages(-1);
  read_active_modules(active_modules, errh);
  read_package_file(clickfs_packages, packages, errh);

  // install required packages
  install_required_packages(r, packages, active_modules, errh);

  // set priority
  if (priority > -100) {
    FILE *f = fopen(clickfs_priority.c_str(), "w");
    if (!f)
      errh->fatal("%s: %s", clickfs_priority.c_str(), strerror(errno));
    fprintf(f, "%d\n", priority);
    fclose(f);
  }

  // write flattened configuration to CLICKFS/config
  int exit_status = 0;
  {
    String config_place = (hotswap ? clickfs_hotconfig : clickfs_config);
    if (verbose)
      errh->message("Writing configuration to %s", config_place.cc());
    int fd = open(config_place.cc(), O_WRONLY | O_TRUNC);
    if (fd < 0)
      errh->fatal("cannot install configuration: %s", strerror(errno));
    // XXX include packages?
    String config = r->configuration_string();
    int pos = 0;
    while (pos < config.length()) {
      ssize_t written = write(fd, config.data() + pos, config.length() - pos);
      if (written >= 0)
	pos += written;
      else if (errno != EAGAIN && errno != EINTR)
	errh->fatal("%s: %s", config_place.cc(), strerror(errno));
    }
    int retval = close(fd);
    if (retval < 0 && errno == EINVAL)
      exit_status = 2;
    else if (retval < 0)
      errh->error("%s: %s", config_place.cc(), strerror(errno));
  }

  // report errors
  {
    char buf[1024];
    int fd = open(clickfs_errors.cc(), O_RDONLY | O_NONBLOCK);
    if (fd < 0)
      errh->warning("%s: %s", clickfs_errors.cc(), strerror(errno));
    else {
      if (verbose)
	errh->message("Waiting for errors");
      while (1) {
	struct timeval wait;
	wait.tv_sec = 0;
	wait.tv_usec = 50000;
	(void) select(0, 0, 0, 0, &wait);
	ssize_t got = read(fd, buf, 1024);
	if (got > 0)
	  fwrite(buf, 1, got, stderr);
	else if (got == 0)
	  break;
	else if (errno != EINTR && errno != EAGAIN) {
	  errh->error("%s: %s", clickfs_errors.cc(), strerror(errno));
	  break;
	}
      }
      close(fd);
    }
  }

  // remove unused packages
  remove_unneeded_packages(active_modules, packages, errh);
  
  if (verbose)
    errh->message("Done");
  exit(exit_status);
}
