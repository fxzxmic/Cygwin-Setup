/*
 * Copyright (c) 2001, 2003 Robert Collins.
 *
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 *
 *     A copy of the GNU General Public License can be found at
 *     http://www.gnu.org/
 *
 * Written by Robert Collins  <rbtcollins@hotmail.com>
 *
 */

#include "package_meta.h"

#include <string>
#include <set>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <strings.h>
#include "getopt++/StringArrayOption.h"

#include "io_stream.h"
#include "compress.h"

#include "filemanip.h"
#include "LogSingleton.h"
/* io_stream needs a bit of tweaking to get rid of this. TODO */
#include "mount.h"
/* this goes at the same time */
#include "win32.h"

#include "script.h"
#include "package_db.h"

#include <algorithm>
#include "Generic.h"
#include "download.h"
#include "Exception.h"
#include "resource.h"

static StringArrayOption DeletePackageOption ('x', "remove-packages", IDS_HELPTEXT_REMOVE_PACKAGES);
static StringArrayOption DeleteCategoryOption ('c', "remove-categories", IDS_HELPTEXT_REMOVE_CATEGORIES);
static StringArrayOption PackageOption ('P', "packages", IDS_HELPTEXT_PACKAGES);
static StringArrayOption CategoryOption ('C', "categories", IDS_HELPTEXT_CATEGORIES);
bool hasManualSelections = 0;

/*****************/

/* Return an appropriate category caption given the action */
unsigned int
packagemeta::action_caption (_actions _value)
{
  switch (_value)
    {
    case NoChange_action:
      return IDS_ACTION_DEFAULT;
    case Install_action:
      return IDS_ACTION_INSTALL;
    case Reinstall_action:
      return IDS_ACTION_REINSTALL;
    case Uninstall_action:
      return IDS_ACTION_UNINSTALL;
    }

  return IDS_ACTION_UNKNOWN;
}

packagemeta::packagemeta (packagemeta const &rhs) :
  name (rhs.name),
  categories (rhs.categories), versions (rhs.versions),
  installed (rhs.installed),
  curr (rhs.curr),
  exp (rhs.exp),
  desired (rhs.desired)
{

}

template<class T> struct removeCategory : public std::unary_function<T, void>
{
  removeCategory(packagemeta *pkg) : _pkg (pkg) {}
  void operator() (T x)
    {
      std::vector <packagemeta *> &aList = packagedb::categories[x];
      aList.erase (find (aList.begin(), aList.end(), _pkg));
    }
  packagemeta *_pkg;
};


packagemeta::~packagemeta()
{
  for_each (categories.begin (), categories.end (), removeCategory<std::string> (this));
  categories.clear ();
  versions.clear ();
}

SolvableVersion
packagemeta::add_version (const SolverPool::addPackageData &inpkgdata)
{
  SolverPool::addPackageData pkgdata = inpkgdata;

  packageversion *v = NULL;
  switch (pkgdata.stability)
    {
    case TRUST_CURR:
      v = &(this->curr);
      break;
    case TRUST_TEST:
      v = &(this->exp);
      break;
    default:
      break;
    }

  /*
    If a packageversion for the same version number is already present, allow
    this version to replace it.

    There is a problem where multiple repos provide a package.  It's never been
    clear which repo should win.  With this implementation, the last one added
    will win.

    We rely on this by adding packages from installed.db last.
   */

  for (std::set <packageversion>::iterator i = versions.begin();
       i != versions.end();
       i++)
    {
      if (i->Canonical_version() != pkgdata.version)
        continue;

      if (pkgdata.vendor == i->Vendor())
        {
          /* Merge the site-list from any existing packageversion with the same
             repository 'release:' label */
          pkgdata.archive.sites.insert(pkgdata.archive.sites.end(),
                                       i->source()->sites.begin(),
                                       i->source()->sites.end());

          /* Installed packages do not supersede repo packages */
          if (pkgdata.reponame != "_installed")
            {
              /* Ensure a stability level doesn't point to a version we're about
                 to remove */
              if (v && (*v == *i))
                *v = packageversion();

              i->remove();
            }
        }
      else
        {
          /* Otherwise... if we had a way to set repo priorities, that could be
             used to control which packageversion the solver picks. For the
             moment, just warn that you might not be getting what you think you
             should...

             (suppress this for installed packages, as we are only guessing the
             vendor, currently)
          */
          if (pkgdata.reponame != "_installed")
            {
              Log (LOG_PLAIN) << "Version " << pkgdata.version << " of package " <<
                name << " is present in releases labelled " << pkgdata.vendor <<
                " and " << i->Vendor() << endLog;
            }
        }

      versions.erase(i);

      break;
    }

  /* Create the SolvableVersion  */
  packagedb db;
  SolvableVersion thepkg = db.solver.addPackage(name, pkgdata);

  /* Add the version */
  std::pair<std::set <packageversion>::iterator, bool> result = versions.insert (thepkg);

  if (!result.second)
    Log (LOG_PLAIN) << "Failed to add version " << thepkg.Canonical_version() << " in package " << name << endLog;
#ifdef DEBUG
  else
    Log (LOG_PLAIN) << "Added version " << thepkg.Canonical_version() << " in package " << name << endLog;
#endif

  /* Record the highest version at a given stability level */
  if (v)
    {
      /* Any version is always greater than no version */
      int comparison = 1;
      if (*v)
        comparison = SolvableVersion::compareVersions(thepkg, *v);

#ifdef DEBUG
      if ((bool)(*v))
        Log (LOG_BABBLE) << "package " << thepkg.Name() << " comparing versions " << thepkg.Canonical_version() << " and " << v->Canonical_version() << ", result was " << comparison << endLog;
#endif

      if (comparison >= 0)
        {
          *v = thepkg;
        }
    }

  return thepkg;
}

const packageversion *
packagemeta::findVersion(std::string &version) const
{
  for (std::set <packageversion>::iterator i = versions.begin();
       i != versions.end();
       i++)
    {
      if (i->Canonical_version() == version)
        return &(*i);
    }

  return NULL;
}

bool
packagemeta::isBlacklisted(const packageversion &version) const
{
  for (std::set<std::string>::iterator i = version_blacklist.begin();
       i != version_blacklist.end();
       i++)
    {
      if (i->compare(version.Canonical_version()) == 0)
        return true;
    }

  return false;
}

void
packagemeta::set_installed_version (const std::string &version)
{
  for (std::set<packageversion>::iterator i = versions.begin(); i != versions.end(); i++)
    {
      if (version.compare(i->Canonical_version()) == 0)
        {
          installed = *i;

          /* and mark as Keep */
          desired = installed;
        }
    }
}

void
packagemeta::add_category (const std::string& cat)
{
  if (categories.find (cat) != categories.end())
    return;
  /* add a new record for the package list */
  packagedb::categories[cat].push_back (this);
  categories.insert (cat);
}

struct StringConcatenator : public std::unary_function<const std::string, void>{
    StringConcatenator(std::string aString) : gap(aString){}
    void operator()(const std::string& aString)
    {
      if (result.size() != 0)
        result += gap;
      result += aString;
    }
    std::string result;
    std::string gap;
};

const std::string
packagemeta::getReadableCategoryList () const
{
  return for_each(categories.begin(), categories.end(),
    visit_if (
      StringConcatenator(", "), bind1st(std::not_equal_to<std::string>(), "All"))
              ).visitor.result;
}

static void
parseNames (std::set<std::string> &parsed, std::string &option)
{
  std::string tname;

  /* Split up the packages listed in the option.  */
  std::string::size_type loc = option.find (",", 0);
  while (loc != std::string::npos)
    {
      tname = option.substr (0, loc);
      option = option.substr (loc + 1);
      parsed.insert (tname);
      loc = option.find (",", 0);
    }

  /* At this point, no "," exists in option.  Don't add
     an empty string if the entire option was empty.  */
  if (option.length ())
    parsed.insert (option);
}

static void
validatePackageNames (std::set<std::string> &names)
{
  packagedb db;
  for (std::set<std::string>::iterator n = names.begin();
       n != names.end();
       ++n)
    {
      if (db.packages.find(*n) == db.packages.end())
        {
          Log(LOG_PLAIN) << "Package '" << *n << "' not found." << endLog;
        }
    }
}

bool packagemeta::isManuallyWanted(packageversion &version) const
{
  static bool parsed_yet = false;
  static std::map<std::string, std::string> parsed_names;
  hasManualSelections |= parsed_names.size ();
  static std::set<std::string> parsed_categories;
  hasManualSelections |= parsed_categories.size ();
  bool bReturn = false;

  /* First time through, we parse all the names out from the
    option string and store them away in an STL set.  */
  if (!parsed_yet)
  {
    std::vector<std::string> packages_options = PackageOption;
    std::vector<std::string> categories_options = CategoryOption;

    std::set<std::string> items;
    for (std::vector<std::string>::iterator n = packages_options.begin ();
		n != packages_options.end (); ++n)
      {
	parseNames (items, *n);
      }

    std::set<std::string> packages;
    /* Separate any 'package=version' into package and version parts */
    for (std::set<std::string>::iterator n = items.begin();
         n != items.end();
         ++n)
      {
        std::string package;
        std::string version;
        std::string::size_type loc = n->find ("=", 0);
        if (loc != std::string::npos)
          {
            package = n->substr(0, loc);
            version = n->substr(loc+1);
          }
        else
          {
            package = *n;
            version = "";
          }
        Log (LOG_BABBLE) << "package: " << package << " version: " << version << endLog;
        parsed_names[package] = version;
        packages.insert(package);
      }

    validatePackageNames (packages);

    for (std::vector<std::string>::iterator n = categories_options.begin ();
		n != categories_options.end (); ++n)
      {
	parseNames (parsed_categories, *n);
      }
    parsed_yet = true;
  }

  /* Once we've already parsed the option string, just do
    a lookup in the cache of already-parsed names.  */
  std::map<std::string, std::string>::iterator i = parsed_names.find(name);
  if (i != parsed_names.end())
    {
      bReturn = true;

      /* Wanted version is unspecified */
      version = packageversion();

      /* ... unless a version was explicitly specified */
      std::string v = i->second;
      if (!v.empty())
        {
          const packageversion *pv = findVersion(v);
          if (pv)
            version = *pv;
          else
            Log (LOG_PLAIN) << "package: " << name << " version: " << v << " not found" << endLog;
        }
    }

  /* If we didn't select the package manually, did we select any
     of the categories it is in? */
  if (!bReturn && parsed_categories.size ())
    {
      std::set<std::string, casecompare_lt_op>::iterator curcat;
      for (curcat = categories.begin (); curcat != categories.end (); curcat++)
	if (parsed_categories.find (*curcat) != parsed_categories.end ())
	  {
	    Log (LOG_BABBLE) << "Found category " << *curcat << " in package " << name << endLog;
	    version = packageversion();
	    bReturn = true;
	  }
    }

  if (bReturn)
    Log (LOG_BABBLE) << "Added manual package " << name << endLog;
  return bReturn;
}

bool packagemeta::isManuallyDeleted() const
{
  static bool parsed_yet = false;
  static std::set<std::string> parsed_delete;
  hasManualSelections |= parsed_delete.size ();
  static std::set<std::string> parsed_delete_categories;
  hasManualSelections |= parsed_delete_categories.size ();
  bool bReturn = false;

  /* First time through, we parse all the names out from the
    option string and store them away in an STL set.  */
  if (!parsed_yet)
  {
    std::vector<std::string> delete_options   = DeletePackageOption;
    std::vector<std::string> categories_options = DeleteCategoryOption;
    for (std::vector<std::string>::iterator n = delete_options.begin ();
		n != delete_options.end (); ++n)
      {
	parseNames (parsed_delete, *n);
      }
    validatePackageNames (parsed_delete);
    for (std::vector<std::string>::iterator n = categories_options.begin ();
		n != categories_options.end (); ++n)
      {
	parseNames (parsed_delete_categories, *n);
      }
    parsed_yet = true;
  }

  /* Once we've already parsed the option string, just do
    a lookup in the cache of already-parsed names.  */
  bReturn = parsed_delete.find(name) != parsed_delete.end();

  /* If we didn't select the package manually, did we select any
     of the categories it is in? */
  if (!bReturn && parsed_delete_categories.size ())
    {
      std::set<std::string, casecompare_lt_op>::iterator curcat;
      for (curcat = categories.begin (); curcat != categories.end (); curcat++)
	if (parsed_delete_categories.find (*curcat) != parsed_delete_categories.end ())
	  {
	    Log (LOG_BABBLE) << "Found category " << *curcat << " in package " << name << endLog;
	    bReturn = true;
	  }
    }

  if (bReturn)
    Log (LOG_BABBLE) << "Deleted manual package " << name << endLog;
  return bReturn;
}

const std::string
packagemeta::SDesc () const
{
  for (std::set<packageversion>::iterator i = versions.begin(); i != versions.end(); i++)
    {
      if (i->SDesc().size())
        return i->SDesc ();
    }

  return std::string();
}

static bool
hasLDesc(packageversion const &pkg)
{
  return pkg.LDesc().size();
}

const std::string
packagemeta::LDesc () const
{
  std::set<packageversion>::iterator i = find_if (versions.begin(), versions.end(), hasLDesc);
  if (i == versions.end())
    return std::string();
  return i->LDesc ();
};

/* Return an appropriate caption given the current action. */
std::wstring
packagemeta::action_caption () const
{
  switch (_action)
    {
    case Uninstall_action:
      return LoadStringW(IDS_ACTION_UNINSTALL);
    case NoChange_action:
      if (!desired)
        return LoadStringW(IDS_ACTION_SKIP);
      if (desired.sourcePackage() && srcpicked())
        /* FIXME: Redo source should come up if the tarball is already present locally */
        return LoadStringW(IDS_ACTION_SOURCE);
      return LoadStringW(IDS_ACTION_KEEP);
    case Reinstall_action:
      return LoadStringW(packagedb::task == PackageDB_Install ? IDS_ACTION_REINSTALL : IDS_ACTION_RETRIEVE);
    case Install_action:
      return string_to_wstring(desired.Canonical_version());
    }
  return LoadStringW(IDS_ACTION_UNKNOWN);
}

void
packagemeta::select_action (int id, trusts const deftrust)
{
  if (id <= 0)
    {
      // Install a specific version
      std::set<packageversion>::iterator i = versions.begin ();
      for (int j = -id; j > 0; j--)
        i++;

      set_action(Install_action, *i, true);
    }
  else
    {
      if (id == packagemeta::NoChange_action)
        set_action((packagemeta::_actions)id, installed);
      else
        set_action((packagemeta::_actions)id, trustp (true, deftrust), true);
    }
}

// toggle between the currently installed version (or uninstalled, if not
// installed), and the naively preferred version (the highest non-test version)
void
packagemeta::toggle_action ()
{
  if (desired != installed)
    {
      set_action(NoChange_action, installed);
    }
  else
    {
      packageversion naively_preferred;
      std::set<packageversion>::iterator i = versions.begin ();
      for (i = versions.begin (); i != versions.end (); ++i)
        if (!packagedb::solver.is_test_package(*i))
          naively_preferred = *i;

      set_action(Install_action, naively_preferred, true);
    }
}

ActionList *
packagemeta::list_actions(trusts const trust)
{
  // build the list of possible actions
  ActionList *al = new ActionList();

  al->add(IDS_ACTION_UNINSTALL, (int)Uninstall_action, (_action == Uninstall_action), bool(installed));
  al->add(IDS_ACTION_SKIP, (int)NoChange_action, (_action == NoChange_action) && !installed, !installed);

  std::set<packageversion>::iterator i;
  for (i = versions.begin (); i != versions.end (); ++i)
    {
      if (*i == installed)
        {
          al->add(IDS_ACTION_KEEP, (int)NoChange_action, (_action == NoChange_action), TRUE);
          al->add(packagedb::task == PackageDB_Install ? IDS_ACTION_REINSTALL : IDS_ACTION_RETRIEVE,
                  (int)Reinstall_action, (_action == Reinstall_action), TRUE);
        }
      else
        {
          std::wstring label = string_to_wstring(i->Canonical_version());
          if (packagedb::solver.is_test_package(*i))
            label += L" (Test)";
          al->add(label,
                  -std::distance(versions.begin (), i),
                  (_action == Install_action) && (*i == desired),
                  TRUE);
        }
    }

  return al;
}

// Set a particular type of action.
void
packagemeta::set_action (_actions action, packageversion const &default_version,
                         bool useraction)
{
  if (action == NoChange_action)
    {
      // if installed, keep
      if (installed
	  || categories.find ("Base") != categories.end ()
	  || categories.find ("Orphaned") != categories.end ())
	{
	  desired = default_version;
	  if (desired)
	    {
	      pick (desired != installed);
	      srcpick (false);
	    }
	}
      else
        {
          // else, if not installed, skip
          desired = packageversion ();
          pick(false);
        }
    }
  else if (action == Install_action)
    {
      desired = default_version;
      if (desired)
	{
	  if (desired != installed)
	    if (desired.accessible ())
	      {
		/* Memorize the fact that the user picked to install this package at least once. */
		if (useraction)
		  user_picked = true;

		pick (true);
		srcpick (false);
	      }
	    else
	      {
		pick (false);
		srcpick (true);
	      }
	  else
	    {
	      action = NoChange_action;
	      pick (false);
	      srcpick (false);
	    }
	}
    }
  else if (action == Reinstall_action)
    {
      desired = installed;
      if (desired)
	{
	  pick (true);
	  srcpick (false);
	}
      else
      {
        action = NoChange_action;
        pick (false);
        srcpick (false);
      }
    }
  else if (action == Uninstall_action)
    {
      desired = packageversion ();
    }

  _action = action;
}

bool
packagemeta::picked () const
{
  return _picked;
}

void
packagemeta::pick (bool picked)
{
  _picked = picked;

  // side effect: display message when picked (if not already seen)
  if (picked)
    this->message.display ();
}

bool
packagemeta::srcpicked () const
{
  return _srcpicked;
}

void
packagemeta::srcpick (bool picked)
{
  _srcpicked = picked;
}

bool
packagemeta::accessible () const
{
  for (std::set<packageversion>::iterator i=versions.begin();
       i != versions.end(); ++i)
    if (i->accessible())
      return true;
  return false;
}

bool
packagemeta::sourceAccessible () const
{
  for (std::set<packageversion>::iterator i=versions.begin();
       i != versions.end(); ++i)
    {
      packageversion bin=*i;
      if (bin.sourcePackage().accessible())
        return true;
    }

  return false;
}

bool
packagemeta::isBinary () const
{
  for (std::set<packageversion>::iterator i=versions.begin();
       i != versions.end(); ++i)
    if ((i->Type() == package_binary) && (i->accessible() || (*i == installed)))
      return true;

  return false;
}

void
packagemeta::logAllVersions () const
{
    for (std::set<packageversion>::iterator i = versions.begin();
	 i != versions.end(); ++i)
      {
	Log (LOG_BABBLE) << "    [" << trustLabel(*i) <<
	  "] ver=" << i->Canonical_version() << endLog;
        std::ostream & logger = Log (LOG_BABBLE);
        logger << "      depends=";
        dumpPackageDepends(i->depends(), logger);
        logger << endLog;
      }
#if 0
    Log (LOG_BABBLE) << "      inst=" << i->
      /* FIXME: Reinstate this code, but spit out all mirror sites */

      for (int t = 1; t < NTRUST; t++)
	{
	  if (pkg->info[t].install)
	    Log (LOG_BABBLE) << "     [%s] ver=%s\n"
		 "          inst=%s %d exists=%s\n"
		 "          src=%s %d exists=%s",
		 infos[t],
		 pkg->info[t].version ? : "(none)",
		 pkg->info[t].install ? : "(none)",
		 pkg->info[t].install_size,
		 (pkg->info[t].install_exists) ? "yes" : "no",
		 pkg->info[t].source ? : "(none)",
		 pkg->info[t].source_size,
		 (pkg->info[t].source_exists) ? "yes" : "no");
	}
#endif
}

std::string
packagemeta::trustLabel(packageversion const &aVersion) const
{
    if (aVersion == curr)
	return "Curr";
    if (aVersion == exp)
	return "Test";
    return "Unknown";
}

void
packagemeta::logSelectionStatus() const
{
  packagemeta const & pkg = *this;
  const char *trust = ((pkg.desired == pkg.curr) ? "curr"
               : (pkg.desired == pkg.exp) ? "test" : "unknown");
  const std::string installed =
   pkg.installed ? pkg.installed.Canonical_version () : "none";

  Log (LOG_BABBLE) << "[" << pkg.name << "] action=" << _action << " trust=" << trust << " installed=" << installed << " src?=" << (pkg.desired && srcpicked() ? "yes" : "no") << endLog;
  if (pkg.categories.size ())
    Log (LOG_BABBLE) << "     categories=" << for_each(pkg.categories.begin(), pkg.categories.end(), StringConcatenator(", ")).result << endLog;
#if 0
  if (pkg.desired.required())
  {
    /* List other packages this package depends on */
      Dependency *dp = pkg.desired->required;
    std::string requires = dp->package.serialise ();
    for (dp = dp->next; dp; dp = dp->next)
       requires += std::string (", ") + dp->package.serialise ();

   Log (LOG_BABBLE) << "     requires=" << requires;
    }
#endif
  pkg.logAllVersions();
}

/* scan for local copies of package */
bool
packagemeta::scan (const packageversion &pkg, bool mirror_mode)
{
  /* empty version */
  if (!pkg)
    return false;

  try
    {
      if (!check_for_cached (*(pkg.source ()), NULL, mirror_mode, false)
          && ::source == IDC_SOURCE_LOCALDIR)
        return false;
    }
  catch (Exception * e)
    {
      // We can ignore these, since we're clearing the source list anyway
      if (e->errNo () == APPERR_CORRUPT_PACKAGE)
        return false;

      // Unexpected exception.
      throw e;
    }

  return true;
}

void
packagemeta::ScanDownloadedFiles (bool mirror_mode)
{
  /* Look at every known package, in all the known mirror dirs,
   * and fill in the Cached attribute if it exists.
   */
  packagedb db;
  for (packagedb::packagecollection::iterator n = db.packages.begin ();
       n != db.packages.end (); ++n)
    {
      packagemeta & pkg = *(n->second);
      std::set<packageversion>::iterator i = pkg.versions.begin ();
      while (i != pkg.versions.end ())
	{
	  /* scan doesn't alter operator == for packageversions */
	  bool lazy_scan = mirror_mode
			   && (*i != pkg.installed
			       || pkg.installed == pkg.curr
			       || pkg.installed == pkg.exp);
	  bool accessible = scan (*i, lazy_scan);
	  packageversion foo = *i;
	  packageversion pkgsrcver = foo.sourcePackage ();
	  bool src_accessible = scan (pkgsrcver, lazy_scan);

	  /* For local installs, if there is no src and no bin, the version
	   * is unavailable
	   */
	  if (!accessible && !src_accessible
	      && *i != pkg.installed)
	    {
	      if (pkg.curr == *i)
		pkg.curr = packageversion ();
	      if (pkg.exp == *i)
		pkg.exp = packageversion ();

	      i->remove();
	      pkg.versions.erase (i++);

	      /* For now, leave the source version alone */
	    }
	  else
	    ++i;
	}
    }
    /* Don't explicity iterate through sources - any sources that aren't
       referenced are unselectable anyway.  */
}

void
packagemeta::addToCategoryBase()
{
  add_category ("Base");
}

bool
packagemeta::hasNoCategories() const
{
  return categories.size() == 0;
}

void
packagemeta::setDefaultCategories()
{
  add_category ("Orphaned");
}

void
packagemeta::addToCategoryAll()
{
  add_category ("All");
}

void
packagemeta::addScript(Script const &aScript)
{
  scripts_.push_back(aScript);
}

std::vector <Script> &
packagemeta::scripts()
{
  return scripts_;
}
