/* Copyright (c) 2015, 2016, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "dd_tablespace.h"

#include <stddef.h>
#include <string>

#include "dd/cache/dictionary_client.h"       // dd::cache::Dictionary_client
#include "dd/dd.h"                            // dd::create_object
#include "dd/dictionary.h"                    // dd::Dictionary::is_dd_table...
#include "dd/impl/system_registry.h"          // dd::System_tablespaces
#include "dd/object_id.h"
#include "dd/properties.h"                    // dd::Properties
#include "dd/types/partition.h"               // dd::Partition
#include "dd/types/table.h"                   // dd::Table
#include "dd/types/tablespace.h"              // dd::Tablespace
#include "dd/types/tablespace_file.h"         // dd::Tablespace_file
#include "handler.h"
#include "my_dbug.h"
#include "my_global.h"
#include "my_sys.h"
#include "mysqld_error.h"
#include "sql_class.h"                        // THD
#include "sql_plugin_ref.h"
#include "sql_table.h"                        // validate_comment_length
#include "table.h"
#include "transaction.h"                      // trans_commit

namespace dd {

bool
fill_table_and_parts_tablespace_names(THD *thd,
                                      const char *db_name,
                                      const char *table_name,
                                      Tablespace_hash_set *tablespace_set)
{
  // Get hold of the dd::Table object.
  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
  const dd::Table *table_obj= NULL;
  if (thd->dd_client()->acquire(db_name, table_name, &table_obj))
  {
    // Error is reported by the dictionary subsystem.
    return(true);
  }

  if (table_obj == NULL)
  {
    /*
      A non-existing table is a perfectly valid scenario, e.g. for
      statements using the 'IF EXISTS' clause. Thus, we cannot throw
      an error in this situation, we just continue returning succuss.
    */
    return false;
  }

  // Add the tablespace name used by dd::Table.
  const char *tablespace= NULL;
  if(!get_tablespace_name<dd::Table>(
       thd, table_obj, &tablespace, thd->mem_root))
  {
    if (tablespace &&
        tablespace_set->insert(const_cast<char*>(tablespace)))
      return true;
  }

  /*
    Add tablespaces used by partition/subpartition definitions
    Note that dd::Table::partitions() gives use both partitions
    and sub-partitions.
   */
  if (table_obj->partition_type() != dd::Table::PT_NONE)
  {
    // Iterate through tablespace names used by partition.
    String_type ts_name;
    for (const dd::Partition *part_obj : table_obj->partitions())
    {
      const char *tablespace= NULL;
      if(!get_tablespace_name<dd::Partition>(
           thd, part_obj, &tablespace, thd->mem_root))
      {
        if (tablespace &&
            tablespace_set->insert(const_cast<char*>(tablespace)))
          return true;
      }

    }
  }

  return false;
}


template <typename T>
bool get_tablespace_name(THD *thd, const T *obj,
                         const char **tablespace_name,
                         MEM_ROOT *mem_root)
{
  //
  // Read Tablespace
  //
  String_type name;

  if (System_tablespaces::instance()->find(MYSQL_TABLESPACE_NAME.str) &&
      dd::get_dictionary()->is_dd_table_name(MYSQL_SCHEMA_NAME.str,
                                             obj->name()))
  {
    // If this is a DD table, and we have a DD tablespace, then we use its name.
    name= MYSQL_TABLESPACE_NAME.str;
  }
  else if (obj->tablespace_id() != dd::INVALID_OBJECT_ID)
  {
    /*
      We get here, when we have InnoDB or NDB table in a tablespace
      which is not one of special 'innodb_%' tablespaces.

      We cannot take MDL lock as we don't know the tablespace name.
      Without a MDL lock we cannot acquire a object placing it in DD
      cache. So we are acquiring the object uncached.

      Note that in theory the fact that we are opening a table in
      some tablespace means that this tablespace can't be dropped
      or created concurrently, so in theory we hold implicit IS
      lock on tablespace (similarly to how it happens for schemas).
    */
    dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
    dd::Tablespace* tablespace= NULL;
    if (thd->dd_client()->acquire_uncached(obj->tablespace_id(), &tablespace))
    {
      // acquire() always fails with a error being reported.
      return true;
    }

    // Report error if tablespace not found.
    if (!tablespace)
    {
      my_error(ER_INVALID_DD_OBJECT_ID, MYF(0), obj->tablespace_id());
      return true;
    }

    name= tablespace->name();
  }
  else
  {
    /*
      If user has specified special tablespace name like 'innodb_%'
      then we read it from tablespace options.
    */
    const dd::Properties *table_options= &obj->options();
    table_options->get("tablespace", name);
  }

  *tablespace_name= NULL;
  if (!name.empty() && !(*tablespace_name= strmake_root(mem_root,
                                             name.c_str(),
                                             name.length())))
  {
    return true;
  }

  return false;
}


dd::Tablespace*
create_tablespace(THD *thd, st_alter_tablespace *ts_info,
                  handlerton *hton, bool commit_dd_changes)
{
  DBUG_ENTER("dd_create_tablespace");

  // Check if same tablespace already exists.
  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
  const dd::Tablespace* ts= NULL;
  if (thd->dd_client()->acquire(ts_info->tablespace_name, &ts))
  {
    // Error is reported by the dictionary subsystem.
    DBUG_RETURN(nullptr);
  }
  if (ts)
  {
    my_error(ER_TABLESPACE_EXISTS, MYF(0), ts_info->tablespace_name);
    DBUG_RETURN(nullptr);
  }

  // Create new tablespace.
  std::unique_ptr<dd::Tablespace> tablespace(dd::create_object<dd::Tablespace>());

  // Set tablespace name
  tablespace->set_name(ts_info->tablespace_name);

  // Engine type
  tablespace->set_engine(ha_resolve_storage_engine_name(hton));

  // Comment
  if (ts_info->ts_comment)
  {
    LEX_CSTRING comment= { ts_info->ts_comment, strlen(ts_info->ts_comment) };

    if (validate_comment_length(thd, comment.str, &comment.length,
                                TABLESPACE_COMMENT_MAXLEN,
                                ER_TOO_LONG_TABLESPACE_COMMENT,
                                ts_info->tablespace_name))
      DBUG_RETURN(nullptr);

    tablespace->set_comment(String_type(comment.str, comment.length));
  }

  if (strlen(ts_info->data_file_name) > FN_REFLEN)
  {
    my_error(ER_PATH_LENGTH, MYF(0), "DATAFILE");
    DBUG_RETURN(nullptr);
  }

  // Add datafile
  dd::Tablespace_file *tsf_obj= tablespace->add_file();
  tsf_obj->set_filename(ts_info->data_file_name);

  Disable_gtid_state_update_guard disabler(thd);

  // Write changes to dictionary.
  if (thd->dd_client()->store(tablespace.get()))
  {
    if (commit_dd_changes)
    {
      trans_rollback_stmt(thd);
      // Full rollback in case we have THD::transaction_rollback_request.
      trans_rollback(thd);
    }
    DBUG_RETURN(nullptr);
  }

  if (commit_dd_changes &&
      (trans_commit_stmt(thd) || trans_commit(thd)))
    DBUG_RETURN(nullptr);

  thd->dd_client()->register_uncommitted_object(tablespace.get());

  DBUG_RETURN(tablespace.release());
}


bool drop_tablespace(THD *thd, const dd::Tablespace* tablespace,
                     bool commit_dd_changes, bool uncached)
{
  DBUG_ENTER("dd_drop_tablespace");

  Disable_gtid_state_update_guard disabler(thd);

  // Drop tablespace
  if ((uncached ? thd->dd_client()->drop_uncached(tablespace) :
                  thd->dd_client()->drop(tablespace)))
  {
    if (commit_dd_changes)
    {
      trans_rollback_stmt(thd);
      // Full rollback in case we have THD::transaction_rollback_request.
      trans_rollback(thd);
    }
    DBUG_RETURN(true);
  }

  DBUG_RETURN(commit_dd_changes &&
              (trans_commit_stmt(thd) || trans_commit(thd)));
}


bool update_tablespace(THD *thd, const dd::Tablespace *old_tablespace,
                       dd::Tablespace *tablespace,
                       bool commit_dd_changes)
{
  DBUG_ENTER("dd_update_tablespace");

  Disable_gtid_state_update_guard disabler(thd);

  if (thd->dd_client()->update<dd::Tablespace>(&old_tablespace,
                                               tablespace))
  {
    if (commit_dd_changes)
    {
      trans_rollback_stmt(thd);
      // Full rollback in case we have THD::transaction_rollback_request.
      trans_rollback(thd);
    }
    DBUG_RETURN(true);
  }

  DBUG_RETURN(commit_dd_changes &&
              (trans_commit_stmt(thd) || trans_commit(thd)));
}

// ALTER TABLESPACE is only supported by NDB for now.
/* purecov: begin deadcode */
bool alter_tablespace(THD *thd, st_alter_tablespace *ts_info,
                      const dd::Tablespace *old_ts_def,
                      dd::Tablespace *new_ts_def)
{
  DBUG_ENTER("dd_alter_tablespace");

  switch (ts_info->ts_alter_tablespace_type)
  {

  // Add data file into a tablespace.
  case ALTER_TABLESPACE_ADD_FILE:
  {
    if (strlen(ts_info->data_file_name) > FN_REFLEN)
    {
      my_error(ER_PATH_LENGTH, MYF(0), "DATAFILE");
      DBUG_RETURN(true);
    }

    dd::Tablespace_file *tsf_obj= new_ts_def->add_file();

    tsf_obj->set_filename(ts_info->data_file_name);
    break;
  }

  // Drop data file from a tablespace.
  case ALTER_TABLESPACE_DROP_FILE:
    if (new_ts_def->remove_file(ts_info->data_file_name))
    {
      my_error(ER_WRONG_FILE_NAME, MYF(0), ts_info->data_file_name, 0, "");
      DBUG_RETURN(true);
    }
    break;

  default:
    my_error(ER_UNKNOWN_ERROR, MYF(0));
    DBUG_RETURN(true);
  }

  DBUG_RETURN(false);
}
/* purecov: end */

} // namespace dd
