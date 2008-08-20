/*
 * Copyright (c) 2007 Brian Dessent
 *
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 *
 *     A copy of the GNU General Public License can be found at
 *     http://www.gnu.org/
 *
 * Written by Brian Dessent <brian@dessent.net>
 *
 */

#if 0
static const char *cvsid =
  "\n%%% $Id$\n";
#endif

#include "win32.h"
#include <memory>
#include <malloc.h>
#include "LogFile.h"

NTSecurity nt_sec;

void
TokenGroupCollection::populate ()
{
  if (!GetTokenInformation (token.theHANDLE(), TokenGroups, buffer,
                            bufferSize, &bufferSize))
    {
      log (LOG_TIMESTAMP) << "GetTokenInformation() failed: " <<
	  	GetLastError () << endLog;
	return;
    }
  populated_ = true;
}

bool
TokenGroupCollection::find (SIDWrapper const &aSID) const
{
  if (!populated ())
    return false;
  TOKEN_GROUPS *groups = (TOKEN_GROUPS *) buffer;
  for (DWORD pg = 0; pg < groups->GroupCount; ++pg)
    if (EqualSid (groups->Groups[pg].Sid, aSID.theSID ()))
      return true;
  return false;
}

void
NTSecurity::SetPosixPerms (const char *fname, HANDLE fh, mode_t mode)
{
  struct {
    SECURITY_DESCRIPTOR sd;
    char sidbuf[2 * MAX_SID_LEN];
  } in_sd;
  SECURITY_DESCRIPTOR out_sd;
  DWORD len, attribute;
  BOOL dummy;
  PSID owner_sid = NULL;
  PSID group_sid = NULL;
  struct {
    ACL acl;
    char aclbuf[4 * (sizeof (ACCESS_ALLOWED_ACE) + MAX_SID_LEN)];
  } acl;

  if (!IsWindowsNT () || !wellKnownSIDsinitialized ())
    return;
  /* Get file's owner and group. */
  if (!GetKernelObjectSecurity (fh, OWNER_SECURITY_INFORMATION
				    | GROUP_SECURITY_INFORMATION,
				&in_sd.sd, sizeof in_sd, &len))
    {
      log (LOG_TIMESTAMP) << "GetKernelObjectSecurity(" << fname << ") failed: "
			  << GetLastError () << endLog;
      return;
    }
  if (!GetSecurityDescriptorOwner (&in_sd.sd, &owner_sid, &dummy))
    log (LOG_TIMESTAMP) << "GetSecurityDescriptorOwner(" << fname
    			<< ") failed: " << GetLastError () << endLog;
  if (!GetSecurityDescriptorGroup (&in_sd.sd, &group_sid, &dummy))
    log (LOG_TIMESTAMP) << "GetSecurityDescriptorGroup(" << fname
    			<< ") failed: " << GetLastError () << endLog;

  /* Initialize out SD */
  if (!InitializeSecurityDescriptor (&out_sd, SECURITY_DESCRIPTOR_REVISION))
    log (LOG_TIMESTAMP) << "InitializeSecurityDescriptor(" << fname
    			<< ") failed: " << GetLastError () << endLog;
  if (OSMajorVersion () >= 5)
    out_sd.Control |= SE_DACL_PROTECTED;

  /* Initialize ACL and fill with almost POSIX-like permissions.
     Note that the current user always requires write permissions, otherwise
     creating files in directories with restricted permissions fails. */
  if (!InitializeAcl (&acl.acl , sizeof acl, ACL_REVISION))
    log (LOG_TIMESTAMP) << "InitializeAcl(" << fname << ") failed: "
    			<< GetLastError () << endLog;
  attribute = STANDARD_RIGHTS_ALL | FILE_GENERIC_READ | FILE_GENERIC_WRITE;
  if (mode & 0100) // S_IXUSR
    attribute |= FILE_GENERIC_EXECUTE;
  if ((mode & 0300) == 0300) // S_IWUSR | S_IXUSR
    attribute |= FILE_DELETE_CHILD;
  if (!AddAccessAllowedAce (&acl.acl, ACL_REVISION, attribute, owner_sid))
    log (LOG_TIMESTAMP) << "AddAccessAllowedAce(" << fname
    			<< ", owner) failed: " << GetLastError () << endLog;
  attribute = STANDARD_RIGHTS_READ | FILE_READ_ATTRIBUTES;
  if (mode & 0040) // S_IRGRP
    attribute |= FILE_GENERIC_READ;
  if (mode & 0020) // S_IWGRP
    attribute |= FILE_GENERIC_WRITE;
  if (mode & 0010) // S_IXGRP
    attribute |= FILE_GENERIC_EXECUTE;
  if ((mode & 01030) == 00030) // S_IWGRP | S_IXGRP, !S_ISVTX
    attribute |= FILE_DELETE_CHILD;
  if (!AddAccessAllowedAce (&acl.acl, ACL_REVISION, attribute, group_sid))
    log (LOG_TIMESTAMP) << "AddAccessAllowedAce(" << fname
    			<< ", group) failed: " << GetLastError () << endLog;
  attribute = STANDARD_RIGHTS_READ | FILE_READ_ATTRIBUTES;
  if (mode & 0004) // S_IROTH
    attribute |= FILE_GENERIC_READ;
  if (mode & 0002) // S_IWOTH
    attribute |= FILE_GENERIC_WRITE;
  if (mode & 0001) // S_IXOTH
    attribute |= FILE_GENERIC_EXECUTE;
  if ((mode & 01003) == 00003) // S_IWOTH | S_IXOTH, !S_ISVTX
    attribute |= FILE_DELETE_CHILD;
  if (!AddAccessAllowedAce (&acl.acl, ACL_REVISION, attribute,
			    everyOneSID.theSID ()))
    log (LOG_TIMESTAMP) << "AddAccessAllowedAce(" << fname
    			<< ", everyone) failed: " << GetLastError () << endLog;
  if (mode & 07000) /* At least one of S_ISUID, S_ISGID, S_ISVTX */
    {
      attribute = 0;
      if (mode & 04000) // S_ISUID
      	attribute |= FILE_APPEND_DATA;
      if (mode & 02000) // S_ISGID
      	attribute |= FILE_WRITE_DATA;
      if (mode & 01000) // S_ISVTX
      	attribute |= FILE_READ_DATA;
      if (!AddAccessAllowedAce (&acl.acl, ACL_REVISION, attribute,
				nullSID.theSID ()))
	log (LOG_TIMESTAMP) << "AddAccessAllowedAce(" << fname
			    << ", null) failed: " << GetLastError () << endLog;
    }

  /* Set SD's DACL to just created ACL. */
  if (!SetSecurityDescriptorDacl (&out_sd, TRUE, &acl.acl, FALSE))
    log (LOG_TIMESTAMP) << "SetSecurityDescriptorDacl(" << fname
    			<< ") failed: " << GetLastError () << endLog;

  /* Write DACL back to file. */
  if (!SetKernelObjectSecurity (fh, DACL_SECURITY_INFORMATION, &out_sd))
    log (LOG_TIMESTAMP) << "SetKernelObjectSecurity(" << fname << ") failed: "
    			<< GetLastError () << endLog;
}

void
NTSecurity::NoteFailedAPI (const std::string &api)
{
  log (LOG_TIMESTAMP) << api << "() failed: " << GetLastError () << endLog;
}

void
NTSecurity::initialiseWellKnownSIDs ()
{
  SID_IDENTIFIER_AUTHORITY n_sid_auth = { SECURITY_NULL_SID_AUTHORITY };
  /* Get the SID for "NULL" S-1-0-0 */
  if (!AllocateAndInitializeSid (&n_sid_auth, 1, SECURITY_NULL_RID,
				 0, 0, 0, 0, 0, 0, 0, &nullSID.theSID ()))
    {
      NoteFailedAPI ("AllocateAndInitializeSid(null)");
      return;
    }
  SID_IDENTIFIER_AUTHORITY e_sid_auth = { SECURITY_WORLD_SID_AUTHORITY };
  /* Get the SID for "Everyone" S-1-1-0 */
  if (!AllocateAndInitializeSid (&e_sid_auth, 1, SECURITY_WORLD_RID,
				 0, 0, 0, 0, 0, 0, 0, &everyOneSID.theSID ()))
    {
      NoteFailedAPI ("AllocateAndInitializeSid(everyone)");
      return;
    }
  SID_IDENTIFIER_AUTHORITY nt_sid_auth = { SECURITY_NT_AUTHORITY };
  /* Get the SID for "Administrators" S-1-5-32-544 */
  if (!AllocateAndInitializeSid (&nt_sid_auth, 2, SECURITY_BUILTIN_DOMAIN_RID, 
				 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
				 &administratorsSID.theSID ()))
    {
      NoteFailedAPI ("AllocateAndInitializeSid(admins)");
      return;
    }
  /* Get the SID for "Users" S-1-5-32-545 */
  if (!AllocateAndInitializeSid (&nt_sid_auth, 2, SECURITY_BUILTIN_DOMAIN_RID, 
				 DOMAIN_ALIAS_RID_USERS, 0, 0, 0, 0, 0, 0,
				 &usersSID.theSID ()))
    {
      NoteFailedAPI ("AllocateAndInitializeSid(users)");
      return;
    }
  wellKnownSIDsinitialized (true);
}

void
NTSecurity::setDefaultDACL ()
{
  /* To assure that the created files have a useful ACL, the 
     default DACL in the process token is set to full access to
     everyone. This applies to files and subdirectories created
     in directories which don't propagate permissions to child
     objects. 
     To assure that the files group is meaningful, a token primary
     group of None is changed to Users or Administrators.
     This is the fallback if real POSIX permissions don't
     work for some reason. */

  /* Create a buffer which has enough room to contain the TOKEN_DEFAULT_DACL
     structure plus an ACL with one ACE.  */
  size_t bufferSize = sizeof (ACL) + sizeof (ACCESS_ALLOWED_ACE)
                      + GetLengthSid (everyOneSID.theSID ()) - sizeof (DWORD);

  std::auto_ptr<char> buf (new char[bufferSize]);

  /* First initialize the TOKEN_DEFAULT_DACL structure.  */
  PACL dacl = (PACL) buf.get ();

  /* Initialize the ACL for containing one ACE.  */
  if (!InitializeAcl (dacl, bufferSize, ACL_REVISION))
    {
      NoteFailedAPI ("InitializeAcl");
      return;
    }

  /* Create the ACE which grants full access to "Everyone" and store it
     in dacl.  */
  if (!AddAccessAllowedAce
      (dacl, ACL_REVISION, GENERIC_ALL, everyOneSID.theSID ()))
    {
      NoteFailedAPI ("AddAccessAllowedAce");
      return;
    }

  /* Set the default DACL to the above computed ACL. */
  if (!SetTokenInformation (token.theHANDLE(), TokenDefaultDacl, &dacl, 
                            bufferSize))
    NoteFailedAPI ("SetTokenInformation");
}

void
NTSecurity::setBackupPrivileges ()
{
  LUID backup, restore;
  if (!LookupPrivilegeValue (NULL, SE_BACKUP_NAME, &backup))
      NoteFailedAPI ("LookupPrivilegeValue");
  else if (!LookupPrivilegeValue (NULL, SE_RESTORE_NAME, &restore))
      NoteFailedAPI ("LookupPrivilegeValue");
  else
    {
      PTOKEN_PRIVILEGES new_privs;

      new_privs = (PTOKEN_PRIVILEGES) alloca (sizeof (TOKEN_PRIVILEGES)
					      + sizeof (LUID_AND_ATTRIBUTES));
      new_privs->PrivilegeCount = 2;
      new_privs->Privileges[0].Luid = backup;
      new_privs->Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
      new_privs->Privileges[1].Luid = restore;
      new_privs->Privileges[1].Attributes = SE_PRIVILEGE_ENABLED;
      if (!AdjustTokenPrivileges (token.theHANDLE (), FALSE, new_privs,
				  0, NULL, NULL))
	NoteFailedAPI ("AdjustTokenPrivileges");
      else if (GetLastError () == ERROR_NOT_ALL_ASSIGNED)
	log (LOG_TIMESTAMP) << "User has NO backup/restore rights" << endLog;
      else 
	log (LOG_TIMESTAMP) << "User has backup/restore rights" << endLog;
    }
}

void
NTSecurity::setDefaultSecurity ()
{
  /* First initialize the well known SIDs used later on. */
  initialiseWellKnownSIDs ();

  /* Get the processes access token. */
  if (!OpenProcessToken (GetCurrentProcess (),
			 TOKEN_READ | TOKEN_ADJUST_DEFAULT
			 | TOKEN_ADJUST_PRIVILEGES, &token.theHANDLE ()))
    {
      NoteFailedAPI ("OpenProcessToken");
      return;
    }

  /* Set backup and restore privileges if available. */
  setBackupPrivileges ();

  /* If initializing the well-known SIDs didn't work, we're finished here. */
  if (!wellKnownSIDsinitialized ())
    return;

  /* Set the default DACL to all permissions for everyone as a fallback. */
  setDefaultDACL ();

  /* Get the user */
  if (!GetTokenInformation (token.theHANDLE (), TokenUser, &osid, 
			    sizeof osid, &size))
    {
      NoteFailedAPI ("GetTokenInformation");
      return;
    }
  /* Make it the owner */
  if (!SetTokenInformation (token.theHANDLE (), TokenOwner, &osid, 
			    sizeof osid))
    {
      NoteFailedAPI ("SetTokenInformation");
      return;
    }
#if 0
  /* FIXME: Setting the primary group to another group negatively
     influences how the postinstall calls to `mkpasswd -c, mkgroup -c'
     work, if the installing user is a domain user.  The group
     information created by the postinstall script will be plain wrong.
     So, for now, until we find a better solution, we better disable
     setting the primary group. */

  /* Get the token groups */
  if (!GetTokenInformation (token.theHANDLE(), TokenGroups, NULL, 0, &size)
	  && GetLastError () != ERROR_INSUFFICIENT_BUFFER)
    {
      NoteFailedAPI("GetTokenInformation");
      return;
    }
  TokenGroupCollection ntGroups(size, token);
  ntGroups.populate ();
  if (!ntGroups.populated ())
    return;
  /* Set the primary group to one of the above computed SID.  */
  PSID nsid = NULL;
  if (ntGroups.find (usersSID))
    {
      nsid = usersSID.theSID ();
      log (LOG_TIMESTAMP) << "Changing gid to Users" << endLog;
    }
  else if (ntGroups.find (administratorsSID))
    {
      nsid = administratorsSID.theSID ();
      log (LOG_TIMESTAMP) << "Changing gid to Administrators" << endLog;
    }
  if (nsid && !SetTokenInformation (token.theHANDLE (), TokenPrimaryGroup,
                                    &nsid, sizeof nsid))
    NoteFailedAPI ("SetTokenInformation");
#endif
}

VersionInfo::VersionInfo ()
{
  v.dwOSVersionInfoSize = sizeof (OSVERSIONINFO);
  if (GetVersionEx (&v) == 0)
    {
      log (LOG_PLAIN) << "GetVersionEx () failed: " << GetLastError () 
                      << endLog;
      
      /* If GetVersionEx fails we really should bail with an error of some kind,
         but for now just assume we're on NT and continue.  */
      v.dwPlatformId = VER_PLATFORM_WIN32_NT;
    }
}

/* This is the Construct on First Use idiom to avoid static initialization
   order problems.  */
VersionInfo& GetVer ()
{
  static VersionInfo *vi = new VersionInfo ();
  return *vi;
}
