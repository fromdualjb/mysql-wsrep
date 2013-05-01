/*****************************************************************************

Copyright (c) 2013, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file row/row0trunc.cc
TRUNCATE implementation

Created 2013-04-12 Sunny Bains
*******************************************************/

#include "row0mysql.h"
#include "pars0pars.h"
#include "dict0crea.h"
#include "dict0boot.h"
#include "dict0stats.h"
#include "dict0stats_bg.h"
#include "lock0lock.h"
#include "fts0fts.h"
#include "srv0space.h"
#include "srv0start.h"
#include "row0trunc.h"

/**
@param mtr	mini-transaction covering the read
@param pcur	persistent cursor used for reading
@return DB_SUCCESS or error code */
dberr_t
Logger::operator()(mtr_t* mtr, btr_pcur_t* pcur)
{
	ulint			len;
	const byte*		field;
	rec_t*			rec = btr_pcur_get_rec(pcur);
	truncate_t::index_t	index;

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_INDEXES__TYPE, &len);
	ut_ad(len == 4);
	index.m_type = mach_read_from_4(field);

	field = rec_get_nth_field_old(rec, DICT_FLD__SYS_INDEXES__ID, &len);
	ut_ad(len == 8);
	index.m_id = mach_read_from_8(field);

	field = rec_get_nth_field_old(
			rec, DICT_FLD__SYS_INDEXES__PAGE_NO, &len);
	ut_ad(len == 4);
	index.m_root_page_no = mach_read_from_4(field);

	/* For compressed tables we need to store extra meta-data
	required during btr_create(). */
	if (fsp_flags_is_compressed(m_flags)) {

		const dict_index_t* dict_index = find(index.m_id);

		if (dict_index != NULL) {
			index.set(dict_index);
		} else {
			ib_logf(IB_LOG_LEVEL_WARN,
				"Index id "IB_ID_FMT " not found",
				index.m_id);
		}
	}

	m_truncate.m_indexes.push_back(index);

	return(DB_SUCCESS);
}

/**
Drop an index in the table.

@param mtr	mini-transaction covering the read
@param pcur	persistent cursor used for reading
@return DB_SUCCESS or error code */
dberr_t
DropIndex::operator()(mtr_t* mtr, btr_pcur_t* pcur) const
{
	ulint	root_page_no;
	rec_t*	rec = btr_pcur_get_rec(pcur);

	root_page_no = dict_drop_index_tree(rec, pcur, false, mtr);

#ifdef UNIV_DEBUG
	{
		ulint		len;
		const byte*	field;
		ulint		index_type;

		field = rec_get_nth_field_old(
			btr_pcur_get_rec(pcur), DICT_FLD__SYS_INDEXES__TYPE,
			&len);
		ut_ad(len == 4);

		index_type = mach_read_from_4(field);

		if (index_type & DICT_CLUSTERED) {
			/* Clustered index */
			DBUG_EXECUTE_IF("ib_crash_on_drop_of_clust_index",
					log_buffer_flush_to_disk();
					os_thread_sleep(2000000);
					DBUG_SUICIDE(););
		} else if (index_type & DICT_UNIQUE) {
			/* Unique index */
			DBUG_EXECUTE_IF("ib_crash_on_drop_of_uniq_index",
					log_buffer_flush_to_disk();
					os_thread_sleep(2000000);
					DBUG_SUICIDE(););
		} else if (index_type == 0) {
			/* Secondary index */
			DBUG_EXECUTE_IF("ib_crash_on_drop_of_sec_index",
					log_buffer_flush_to_disk();
					os_thread_sleep(2000000);
					DBUG_SUICIDE(););
		}
	}
#endif /* UNIV_DEBUG */

	DBUG_EXECUTE_IF("ib_crash_ibd_file_missing",
			root_page_no = FIL_NULL;);

	if (root_page_no != FIL_NULL) {

		/* We will need to commit and restart the
		mini-transaction in order to avoid deadlocks.
		The dict_drop_index_tree() call has freed
		a page in this mini-transaction, and the rest
		of this loop could latch another index page.*/
		mtr_commit(mtr);

		mtr_start(mtr);

		btr_pcur_restore_position(BTR_SEARCH_LEAF, pcur, mtr);
	} else {
		ulint	zip_size;

		/* Check if the .ibd file is missing. */
		zip_size = fil_space_get_zip_size(m_table->space);

		DBUG_EXECUTE_IF("ib_crash_ibd_file_missing",
				zip_size = ULINT_UNDEFINED;);

		if (zip_size == ULINT_UNDEFINED) {
			return(DB_ERROR);
		}
	}

	return(DB_SUCCESS);
}

/**
Create the new index and update the root page number in the
SysIndex table.

@param mtr	mini-transaction covering the read
@param pcur	persistent cursor used for reading
@return DB_SUCCESS or error code */
dberr_t
CreateIndex::operator()(mtr_t* mtr, btr_pcur_t* pcur) const
{
	ulint	root_page_no;

	root_page_no = dict_recreate_index_tree(m_table, pcur, mtr);

#ifdef UNIV_DEBUG
	{
		ulint		len;
		const byte*	field;
		ulint		index_type;

		field = rec_get_nth_field_old(
			btr_pcur_get_rec(pcur), DICT_FLD__SYS_INDEXES__TYPE,
			&len);
		ut_ad(len == 4);

		index_type = mach_read_from_4(field);

		if (index_type & DICT_CLUSTERED) {
			/* Clustered index */
			DBUG_EXECUTE_IF("ib_crash_on_create_of_clust_index",
					log_buffer_flush_to_disk();
					os_thread_sleep(2000000);
					DBUG_SUICIDE(););
		} else if (index_type & DICT_UNIQUE) {
			/* Unique index */
			DBUG_EXECUTE_IF("ib_crash_on_create_of_uniq_index",
					log_buffer_flush_to_disk();
					os_thread_sleep(2000000);
					DBUG_SUICIDE(););
		} else if (index_type == 0) {
			/* Secondary index */
			DBUG_EXECUTE_IF("ib_crash_on_create_of_sec_index",
					log_buffer_flush_to_disk();
					os_thread_sleep(2000000);
					DBUG_SUICIDE(););
		}
	}
#endif /* UNIV_DEBUG */

	if (root_page_no != FIL_NULL) {

		rec_t*	rec = btr_pcur_get_rec(pcur);

		page_rec_write_field(
			rec, DICT_FLD__SYS_INDEXES__PAGE_NO,
			root_page_no, mtr);

		/* We will need to commit and restart the
		mini-transaction in order to avoid deadlocks.
		The dict_create_index_tree() call has allocated
		a page in this mini-transaction, and the rest of
		this loop could latch another index page. */
		mtr_commit(mtr);

		mtr_start(mtr);

		btr_pcur_restore_position(BTR_MODIFY_LEAF, pcur, mtr);

	} else {
		ulint	zip_size;

		zip_size = fil_space_get_zip_size(m_table->space);

		if (zip_size == ULINT_UNDEFINED) {
			return(DB_ERROR);
		}
	}

	return(DB_SUCCESS);
}

/**
Rollback the transaction and release the index locks.

@param table		table to truncate
@param trx		transaction covering the TRUNCATE */
static
void
row_truncate_rollback(dict_table_t* table, trx_t* trx)
{
	dict_table_x_unlock_indexes(table);

	trx->error_state = DB_SUCCESS;

	trx_rollback_to_savepoint(trx, NULL);

	trx->error_state = DB_SUCCESS;

	table->corrupted = true;
}

/**
Finish the TRUNCATE operations for both commit and rollback.

@param table		table being truncated
@param trx		transaction covering the truncate
@param flags		tablespace flags
@param err		status of truncate operation

@return DB_SUCCESS or error code */
static __attribute__((warn_unused_result))
dberr_t
row_truncate_complete(dict_table_t* table, trx_t* trx, ulint flags, dberr_t err)
{
	row_mysql_unlock_data_dictionary(trx);

	if (!dict_table_is_temporary(table)
	    && flags != ULINT_UNDEFINED
	    && err == DB_SUCCESS) {

		/* Waiting for MLOG_FILE_TRUNCATE record is written into
		redo log before the crash. */
		DBUG_EXECUTE_IF("ib_crash_before_log_checkpoint1",
				DBUG_SUICIDE(););

		DBUG_EXECUTE_IF("ib_crash_before_log_checkpoint2",
				log_buffer_flush_to_disk();
				os_thread_sleep(2000000);
				DBUG_SUICIDE(););

		log_make_checkpoint_at(LSN_MAX, TRUE);

		DBUG_EXECUTE_IF("ib_crash_after_log_checkpoint",
				DBUG_SUICIDE(););

		if (!Tablespace::is_system_tablespace(table->space)) {
			err = truncate_t::truncate(
				table->space, table->name,
				table->data_dir_path, flags, false);
		}
	}

	dict_stats_update(table, DICT_STATS_EMPTY_TABLE);

	trx->op_info = "";

	/* For temporary tables or if there was an error, we need to reset
	the dict operation flags. */
	trx->ddl = false;
	trx->dict_operation = TRX_DICT_OP_NONE;
	ut_ad(trx->state == TRX_STATE_NOT_STARTED);

	srv_wake_master_thread();

	return(err);
}

/**
Handle FTS truncate issues.
@param table		table being truncated
@param new_id		new id for the table
@param trx		transaction covering the truncate
@return DB_SUCCESS or error code. */
static __attribute__((warn_unused_result))
dberr_t
row_truncate_fts(dict_table_t* table, table_id_t new_id, trx_t* trx)
{
	dict_table_t	fts_table;

	fts_table.id = new_id;
	fts_table.name = table->name;

	dberr_t		err;

	err = fts_create_common_tables(trx, &fts_table, table->name, TRUE);

	for (ulint i = 0;
	     i < ib_vector_size(table->fts->indexes) && err == DB_SUCCESS;
	     i++) {

		dict_index_t*	fts_index;

		fts_index = static_cast<dict_index_t*>(
			ib_vector_getp(table->fts->indexes, i));

		err = fts_create_index_tables_low(
			trx, fts_index, table->name, new_id);
	}

	if (err != DB_SUCCESS) {

		trx->error_state = DB_SUCCESS;
		trx_rollback_to_savepoint(trx, NULL);
		trx->error_state = DB_SUCCESS;

		char	table_name[MAX_FULL_NAME_LEN + 1];

		innobase_format_name(
			table_name, sizeof(table_name), table->name, FALSE);

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Unable to truncate FTS index for table %s",
			table_name);

		table->corrupted = true;

	} else {
		ut_ad(trx->state != TRX_STATE_NOT_STARTED);
	}

	return(err);
}

/**
Update system table to reflect new table id.
@param old_table_id		old table id
@param new_table_id		new table id
@param reserve_dict_mutex 	if true, acquire/release
				dict_sys->mutex around call to pars_sql. 
@param trx			transaction
@return error code or DB_SUCCESS */
static __attribute__((warn_unused_result))
dberr_t
row_truncate_update_table_id(
	ulint	old_table_id,
	ulint	new_table_id,
	ibool	reserve_dict_mutex,
	trx_t*	trx)
{
	pars_info_t*	info	= NULL;
	dberr_t		err 	= DB_SUCCESS;

	/* Scan the SYS_XXXX table and update to reflect new table-id. */
	info = pars_info_create();
	pars_info_add_ull_literal(info, "old_id", old_table_id);
	pars_info_add_ull_literal(info, "new_id", new_table_id);

	err = que_eval_sql(
		info,
		"PROCEDURE RENUMBER_TABLE_ID_PROC () IS\n"
		"BEGIN\n"
		"UPDATE SYS_TABLES"
		" SET ID = :new_id\n"
		" WHERE ID = :old_id;\n"
		"UPDATE SYS_COLUMNS SET TABLE_ID = :new_id\n"
		" WHERE TABLE_ID = :old_id;\n"
		"UPDATE SYS_INDEXES"
		" SET TABLE_ID = :new_id\n"
		" WHERE TABLE_ID = :old_id;\n"
		"END;\n", reserve_dict_mutex, trx);

	return(err);
}

/**
Update system table to reflect new table id/new root page number.
@param redo_cache		contains info like old table id
				and updated root_page_no of indexes.
@param new_table_id		new table id
@param reserve_dict_mutex 	if true, acquire/release
				dict_sys->mutex around call to pars_sql. 
@return error code or DB_SUCCESS */
static __attribute__((warn_unused_result))
dberr_t
row_truncate_update_sys_tables_post_redo(
	truncate_redo_cache_t*	redo_cache,
	ulint			new_table_id,
	ibool			reserve_dict_mutex)
{
	dberr_t	err 	= DB_SUCCESS;
	trx_t*	trx;

	trx = trx_allocate_for_background();
	trx_set_dict_operation(trx, TRX_DICT_OP_TABLE);

	/* Step-1: Update the root-page-no */
	truncate_redo_cache_t::index_page_cache_t::const_iterator end =
		redo_cache->m_new_page_no.end();

	for (truncate_redo_cache_t::index_page_cache_t::const_iterator it =
		redo_cache->m_new_page_no.begin();
	     it != end;
	     ++it) {

		pars_info_t*	info	= NULL;
		info = pars_info_create();
		pars_info_add_int4_literal(info, "page_no", it->second);
		pars_info_add_ull_literal(info, "table_id",
			redo_cache->m_old_table_id);
		pars_info_add_ull_literal(info, "index_id", it->first);

		err = que_eval_sql(
			info,
			"PROCEDURE RENUMBER_IDX_PAGE_NO_PROC () IS\n"
			"BEGIN\n"
			"UPDATE SYS_INDEXES"
			" SET PAGE_NO = :page_no\n"
			" WHERE TABLE_ID = :table_id"
			" AND ID = :index_id;\n"
			"END;\n", reserve_dict_mutex, trx);

		if (err != DB_SUCCESS) {
			return(err);
		}
	}
		
	/* Step-2: Update table-id. */
	err = row_truncate_update_table_id(
		redo_cache->m_old_table_id, new_table_id,
		reserve_dict_mutex, trx);

	if (err != DB_SUCCESS) {
		return(err);
	}

	trx_commit_for_mysql(trx);
	trx_free_for_background(trx);

	return(err);
}

/**
Truncatie also results in assignment of new table id, update the system
SYSTEM TABLES with the new id.
@param table,			table being truncated
@param new_id,			new table id
@param old_space,		old space id
@param has_internal_doc_id,	has doc col (fts)
@param trx)			transaction handle
@return	error code or DB_SUCCESS */
static __attribute__((warn_unused_result))
dberr_t
row_truncate_update_system_tables(
	dict_table_t*	table,
	table_id_t	new_id,
	bool		has_internal_doc_id,
	trx_t*		trx)
{
	dberr_t		err	= DB_SUCCESS;

	ut_a(!dict_table_is_temporary(table));

	err = row_truncate_update_table_id(table->id, new_id, FALSE, trx);

	DBUG_EXECUTE_IF("ib_ddl_crash_before_fts_truncate", err = DB_ERROR;);

	if (err != DB_SUCCESS) {
		trx->error_state = DB_SUCCESS;
		trx_rollback_to_savepoint(trx, NULL);
		trx->error_state = DB_SUCCESS;

		/* Update of system table failed. Table in memory metadata
		could be in an inconsistent state, mark the in-memory
		table->corrupted to be true. */
		table->corrupted = true;

		char	table_name[MAX_FULL_NAME_LEN + 1];

		innobase_format_name(
			table_name, sizeof(table_name), table->name, FALSE);

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Unable to assign a new identifier to table %s "
			"after truncating it. Marked the table as corrupted. "
			"In-memory representation is now different from the "
			"on-disk representation.", table_name);

		/* Failed to update the table id, so drop the new
		FTS auxiliary tables */
		if (has_internal_doc_id) {
			ut_ad(trx->state == TRX_STATE_NOT_STARTED);

			table_id_t	id = table->id;

			table->id = new_id;

			fts_drop_tables(trx, table);

			table->id = id;

			ut_ad(trx->state != TRX_STATE_NOT_STARTED);
		}

		err = DB_ERROR;
	} else {
		/* Drop the old FTS index */
		if (has_internal_doc_id) {
			ut_ad(trx->state != TRX_STATE_NOT_STARTED);
			fts_drop_tables(trx, table);
			ut_ad(trx->state != TRX_STATE_NOT_STARTED);
		}

		DBUG_EXECUTE_IF("ib_truncate_crash_after_fts_drop",
				DBUG_SUICIDE(););

		dict_table_change_id_in_cache(table, new_id);

		/* Reset the Doc ID in cache to 0 */
		if (has_internal_doc_id && table->fts->cache != NULL) {
			table->fts->fts_status |= TABLE_DICT_LOCKED;
			fts_update_next_doc_id(trx, table, NULL, 0);
			fts_cache_clear(table->fts->cache, TRUE);
			fts_cache_init(table->fts->cache);
			table->fts->fts_status &= ~TABLE_DICT_LOCKED;
		}
	}

	return(err);
}

/**
Prepare for the truncate process. On success all of the table's indexes will
be locked in X mode.
@param table		table to truncate
@param flags		tablespace flags
@return	error code or DB_SUCCESS */
static __attribute__((warn_unused_result))
dberr_t
row_truncate_prepare(dict_table_t* table, ulint* flags)
{
	ut_ad(!dict_table_is_temporary(table));
	ut_ad(!Tablespace::is_system_tablespace(table->space));

	*flags = fil_space_get_flags(table->space);

	ut_ad(!DICT_TF2_FLAG_IS_SET(table, DICT_TF2_TEMPORARY));

	dict_get_and_save_data_dir_path(table, true);

	if (*flags != ULINT_UNDEFINED) {

		dberr_t	err = fil_prepare_for_truncate(table->space);

		if (err != DB_SUCCESS) {
			return(err);
		}
	}

	return(DB_SUCCESS);
}

/**
Do foreign key checks before starting TRUNCATE.
@param table		table being truncated
@param trx		transaction covering the truncate
@return DB_SUCCESS or error code */
static __attribute__((warn_unused_result))
dberr_t
row_truncate_foreign_key_checks(
	const dict_table_t*	table,
	const trx_t*		trx)
{
	/* Check if the table is referenced by foreign key constraints from
	some other table (not the table itself) */

	dict_foreign_t*	foreign;

	for (foreign = UT_LIST_GET_FIRST(table->referenced_list);
	     foreign != 0 && foreign->foreign_table == table;
	     foreign = UT_LIST_GET_NEXT(referenced_list, foreign)) {

		/* Do nothing. */
	}

	if (!srv_read_only_mode && foreign != NULL && trx->check_foreigns) {

		FILE*	ef = dict_foreign_err_file;

		/* We only allow truncating a referenced table if
		FOREIGN_KEY_CHECKS is set to 0 */

		mutex_enter(&dict_foreign_err_mutex);

		rewind(ef);

		ut_print_timestamp(ef);

		fputs("  Cannot truncate table ", ef);
		ut_print_name(ef, trx, TRUE, table->name);
		fputs(" by DROP+CREATE\n"
		      "InnoDB: because it is referenced by ", ef);
		ut_print_name(ef, trx, TRUE, foreign->foreign_table_name);
		putc('\n', ef);

		mutex_exit(&dict_foreign_err_mutex);

		return(DB_ERROR);
	}

	/* TODO: could we replace the counter n_foreign_key_checks_running
	with lock checks on the table? Acquire here an exclusive lock on the
	table, and rewrite lock0lock.cc and the lock wait in srv0srv.cc so that
	they can cope with the table having been truncated here? Foreign key
	checks take an IS or IX lock on the table. */

	if (table->n_foreign_key_checks_running > 0) {

		char	table_name[MAX_FULL_NAME_LEN + 1];

		innobase_format_name(
			table_name, sizeof(table_name), table->name, FALSE);

		ib_logf(IB_LOG_LEVEL_WARN,
			"Cannot truncate table %s because there is a "
			"foreign key check running on it.", table_name);

		return(DB_ERROR);
	}

	return(DB_SUCCESS);
}

/**
Do some sanity checks before starting the actual TRUNCATE.
@param table		table being truncated
@return DB_SUCCESS or error code */
static __attribute__((warn_unused_result))
dberr_t
row_truncate_sanity_checks(const dict_table_t* table)
{
	if (srv_sys_space.created_new_raw()) {

		ib_logf(IB_LOG_LEVEL_INFO,
			"A new raw disk partition was initialized: "
			"we do not allow database modifications by the "
			"user. Shut down mysqld and edit my.cnf so that "
			"newraw is replaced with raw.");

		return(DB_ERROR);

	} else if (dict_table_is_discarded(table)) {

		return(DB_TABLESPACE_DELETED);

	} else if (table->ibd_file_missing) {

		return(DB_TABLESPACE_NOT_FOUND);
	}

	return(DB_SUCCESS);
}

/**
Truncates a table for MySQL.
@param table		table being truncated
@param trx		transaction covering the truncate
@return	error code or DB_SUCCESS */
UNIV_INTERN
dberr_t
row_truncate_table_for_mysql(dict_table_t* table, trx_t* trx)
{
	dberr_t		err;
	ulint		old_space = table->space;

	/* Understanding the truncate flow.

	Step-1: Perform intiial sanity check to ensure table can be truncated.
	This would include check for tablespace discard status, ibd file
	missing, etc ....

	Step-2: Start transaction (only for non-temp table as temp-table don't
	modify any data on disk doesn't need transaction object).

	Step-3: Validate ownership of needed locks (Exclusive lock).
	Ownership will also ensure there is no active SQL queries, INSERT,
	SELECT, .....

	Step-4: Stop all the background process associated with table.

	Step-5: There are few foreign key related constraint under which
	we can't truncate table (due to referential integrity unless it is
	turned off). Ensure this condition is satisfied.

	Step-6: Truncate operation can be rolled back in case of error
	till some point. Associate rollback segment to record undo log.

	**** Based on whether table reside in system or per-table tablespace
	follow-up steps might turn-conditional.

	Step-7: Generate new table-id.
	Why we need new table-id ?
	Purge and rollback: we assign a new table id for the
	table. Since purge and rollback look for the table based on
	the table id, they see the table as 'dropped' and discard
	their operations.

	Step-8: (Only for per-tablespace): REDO log information about
	tablespace which mainly include index information (id, type).
	In event of crash post this point on recovery using REDO log
	tablespace can be re-created with appropriate index id and type
	information.

	Step-9: Drop all indexes (this include freeing of the pages
	associated with them). (FIXME: freeing of pages should be conditional
	and should be applicable only when using shared tablespaces.)

	Step-10: Re-create new indexes.

	Step-11: Update new table-id to in-memory cache (dictionary),
	on-disk (INNODB_SYS_TABLES). INNODB_SYS_INDEXES also needs to
	get updated to reflect updated page-no of new index created
	and updated table-id. 

	Step-12: Cleanup Stage. Reset auto-inc value to 1.
	Release all the locks.
	Commit the transaction. Update trx operation state.

	FIXME:
	3) Insert buffer: TRUNCATE TABLE is analogous to DROP TABLE,
	so we do not have to remove insert buffer records, as the
	insert buffer works at a low level. If a freed page is later
	reallocated, the allocator will remove the ibuf entries for
	it.

	When we prepare to truncate *.ibd files, we remove all entries
	for the table in the insert buffer tree. This is not strictly
	necessary, but we can free up some space in the system tablespace.

	4) Linear readahead and random readahead: we use the same
	method as in 3) to discard ongoing operations. (This is only
	relevant for TRUNCATE TABLE by TRUNCATE TABLESPACE.)
	Ensure that the table will be dropped by
	trx_rollback_active() in case of a crash.
	*/

	/*-----------------------------------------------------------------*/

	/* Step-1: Perform intiial sanity check to ensure table can be
	truncated. This would include check for tablespace discard status,
	ibd file missing, etc .... */
	err = row_truncate_sanity_checks(table);
	if (err != DB_SUCCESS) {
		return(err);

	}

	log_make_checkpoint_at(LSN_MAX, TRUE);

	/* Step-2: Start transaction (only for non-temp table as temp-table
	don't modify any data on disk doesn't need transaction object). */
	if (!dict_table_is_temporary(table)) {

		/* Avoid transaction overhead for temporary table DDL. */
		trx_start_for_ddl(trx, TRX_DICT_OP_TABLE);
	}

	/* Step-3: Validate ownership of needed locks (Exclusive lock).
	Ownership will also ensure there is no active SQL queries, INSERT,
	SELECT, .....*/
	trx->op_info = "truncating table";
	ut_a(trx->dict_operation_lock_mode == 0);

	row_mysql_lock_data_dictionary(trx);

	ut_ad(mutex_own(&(dict_sys->mutex)));
#ifdef UNIV_SYNC_DEBUG
	ut_ad(rw_lock_own(&dict_operation_lock, RW_LOCK_EX));
#endif /* UNIV_SYNC_DEBUG */

	/* Step-4: Stop all the background process associated with table. */
	dict_stats_wait_bg_to_stop_using_table(table, trx);

	/* Step-5: There are few foreign key related constraint under which
	we can't truncate table (due to referential integrity unless it is
	turned off). Ensure this condition is satisfied. */
	ulint	flags = ULINT_UNDEFINED;
	err = row_truncate_foreign_key_checks(table, trx);
	if (err != DB_SUCCESS) {
		return(row_truncate_complete(table, trx, flags, err));
	}

	/* Remove all locks except the table-level X lock. */
	lock_remove_all_on_table(table, FALSE);
	trx->table_id = table->id;
	trx_set_dict_operation(trx, TRX_DICT_OP_TABLE);

	/* Step-6: Truncate operation can be rolled back in case of error
	till some point. Associate rollback segment to record undo log. */
	if (!dict_table_is_temporary(table)) {

		/* Temporary tables don't need undo logging for autocommit stmt.
		On crash (i.e. mysql restart) temporary tables are anyway not
		accessible. */
		mutex_enter(&trx->undo_mutex);

		err = trx_undo_assign_undo(trx, TRX_UNDO_UPDATE);

		mutex_exit(&trx->undo_mutex);

		if (err != DB_SUCCESS) {
			return(row_truncate_complete(table, trx, flags, err));
		}
	}

	/* Step-7: Generate new table-id.
	Why we need new table-id ?
	Purge and rollback: we assign a new table id for the
	table. Since purge and rollback look for the table based on
	the table id, they see the table as 'dropped' and discard
	their operations. */
	table_id_t	new_id;
	dict_hdr_get_new_id(&new_id, NULL, NULL, table, false);

	/* Step-8: (Only for per-tablespace): REDO log information about
	tablespace which mainly include index information (id, type).
	In event of crash post this point on recovery using REDO log
	tablespace can be re-created with appropriate index id and type
	information. */

	/* Lock all index trees for this table, as we will truncate
	the table/index and possibly change their metadata. All
	DML/DDL are blocked by table level lock, with a few exceptions
	such as queries into information schema about the table,
	MySQL could try to access index stats for this kind of query,
	we need to use index locks to sync up */
	dict_table_x_lock_indexes(table);

	if (!dict_table_is_temporary(table)) {

		if (!Tablespace::is_system_tablespace(table->space)) {

			err = row_truncate_prepare(table, &flags);
			if (err != DB_SUCCESS) {
				return(row_truncate_complete(
					table, trx, flags, err));
			}
		} else {
			flags = fil_space_get_flags(table->space);
		}

		/* Write the TRUNCATE redo log. */
		Logger logger(table, flags, new_id);

		err = SysIndexIterator().for_each(logger);

		ut_ad(err == DB_SUCCESS);

		ut_ad(logger.debug());

		/* Write the TRUNCATE log record into redo log */
		logger.log();
	}

	/* This is the event horizon, if an error occurs after this point then
	the table will be tagged as corrupt in memory. On restart we will
	recreate the table as per REDO semantics from the REDO log record
	that we wrote above provided REDO log is flushed to disk. If REDO logged
	is not yet flushed to disk and crash occur then we restore the table
	in old state with rows. */

	DBUG_EXECUTE_IF("ib_crash_after_redo_log_write_complete1",
			DBUG_SUICIDE(););

	DBUG_EXECUTE_IF("ib_crash_after_redo_log_write_complete2",
			log_buffer_flush_to_disk();
			os_thread_sleep(3000000);
			DBUG_SUICIDE(););

	/* Step-9: Drop all indexes (this include freeing of the pages
	associated with them). */
	if (!dict_table_is_temporary(table)) {

		/* Drop all the indexes. */
		DropIndex	dropIndex(table);

		err = SysIndexIterator().for_each(dropIndex);

		if (err != DB_SUCCESS) {
			row_truncate_rollback(table, trx);
			return(row_truncate_complete(table, trx, flags, err));
		}

	} else {
		/* For temporary tables we don't have entries in
		SYSTEM TABLES. */
		for (dict_index_t* index = UT_LIST_GET_FIRST(table->indexes);
		     index != NULL;
		     index = UT_LIST_GET_NEXT(indexes, index)) {

			dict_truncate_index_tree_in_mem(index);
		}
	}

	if (!Tablespace::is_system_tablespace(table->space)
	    && !dict_table_is_temporary(table)
	    && flags != ULINT_UNDEFINED) {

		fil_reinit_space_header(
			table->space,
			table->indexes.count + FIL_IBD_FILE_INITIAL_SIZE + 1);
	}

	DBUG_EXECUTE_IF("ib_crash_drop_reinit_done_create_to_start1",
			DBUG_SUICIDE(););
	DBUG_EXECUTE_IF("ib_crash_drop_reinit_done_create_to_start2",
			log_buffer_flush_to_disk();
			os_thread_sleep(2000000);
			DBUG_SUICIDE(););

	/* Step-10: Re-create new indexes. */
	if (!dict_table_is_temporary(table)) {

		/* Recreate all the indexes. */
		CreateIndex	createIndex(table);

		err = SysIndexIterator().for_each(createIndex);

		if (err != DB_SUCCESS) {
			row_truncate_rollback(table, trx);
			return(row_truncate_complete(table, trx, flags, err));
		}
	}

	/* Done with index truncation, release index tree locks,
	subsequent work relates to table level metadata change */
	dict_table_x_unlock_indexes(table);

	/* Create new FTS auxiliary tables with the new_id, and
	drop the old index later, only if everything runs successful. */
	bool	has_internal_doc_id =
		dict_table_has_fts_index(table)
		|| DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_HAS_DOC_ID);

	if (has_internal_doc_id) {

		err = row_truncate_fts(table, new_id, trx);

		if (err != DB_SUCCESS) {

			return(row_truncate_complete(table, trx, flags, err));
		}
	}

	/* Step-11: Update new table-id to in-memory cache (dictionary),
	on-disk (INNODB_SYS_TABLES). INNODB_SYS_INDEXES also needs to
	get updated to reflect updated page-no of new index created
	and updated table-id. */
	if (dict_table_is_temporary(table)) {

		dict_table_change_id_in_cache(table, new_id);
		err = DB_SUCCESS;

	} else {

		/* If this fails then we are in an inconsistent state and
		the results are undefined. */
		ut_ad(old_space == table->space);
		err = row_truncate_update_system_tables(
			table, new_id, has_internal_doc_id, trx);

		if (err != DB_SUCCESS) {
			table->corrupted = true;
		}
	}

	DBUG_EXECUTE_IF("ib_crash_on_updating_dict_sys_info1", DBUG_SUICIDE(););
	DBUG_EXECUTE_IF("ib_crash_on_updating_dict_sys_info2",
			log_buffer_flush_to_disk();
			os_thread_sleep(2000000);
			DBUG_SUICIDE(););

	/* Step-12: Cleanup Stage. Reset auto-inc value to 1.
	Release all the locks.
	Commit the transaction. Update trx operation state. */
	dict_table_autoinc_lock(table);
	dict_table_autoinc_initialize(table, 1);
	dict_table_autoinc_unlock(table);

	if (trx->state != TRX_STATE_NOT_STARTED) {
		trx_commit_for_mysql(trx);
	}

	return(row_truncate_complete(table, trx, flags, err));
}

/**
Assign new table-id to table that got truncated as part of REDO log
apply phase.
@return	error code or DB_SUCCESS */
UNIV_INTERN
dberr_t
row_complete_truncate_of_tables()
{
	dberr_t	err = DB_SUCCESS;

	std::vector<truncate_redo_cache_t*> redo_cache;

	/* Apply truncate REDO log action. */
	for (ulint i = 0; i < srv_tables_to_truncate.size(); i++) {
		truncate_t* tbl = srv_tables_to_truncate[i];

		truncate_redo_cache_t* redo_cache_entry =
			new truncate_redo_cache_t(
				tbl->m_old_table_id, tbl->m_new_table_id);

		if (!Tablespace::is_system_tablespace(tbl->m_space_id)) {

			if (!fil_tablespace_exists_in_mem(tbl->m_space_id)) {

				/* Create the database directory for name,
				if it does not exist yet */
				fil_create_directory_for_tablename(
					tbl->m_tablename);

				if (fil_create_new_single_table_tablespace(
						tbl->m_space_id,
						tbl->m_tablename,
						tbl->m_dir_path,
						tbl->m_tablespace_flags,
						DICT_TF2_USE_TABLESPACE,
						FIL_IBD_FILE_INITIAL_SIZE)
					!= DB_SUCCESS) {

					/* If checkpoint is not yet done
					and table is dropped and then we might
					still have REDO entries for this table
					which are INVALID. Ignore them. */
					ib_logf(IB_LOG_LEVEL_INFO,
						"Failed to create tablespace"
						" for %lu space-id\n",
						tbl->m_space_id);
					err = DB_ERROR;
					break;
				}
			}

			ut_ad(fil_tablespace_exists_in_mem(tbl->m_space_id)
				== TRUE);

			fil_recreate_tablespace(
				tbl->m_space_id,
				tbl->m_format_flags,
				tbl->m_tablespace_flags,
				tbl->m_tablename,
				*tbl, log_get_lsn(), redo_cache_entry);

		} else if (Tablespace::is_system_tablespace(tbl->m_space_id)) {

			/* Only tables residing in ibdata1 are truncated.
			Temp-tables in temp-tablespace are never restored.*/
			ut_ad(tbl->m_space_id == srv_sys_space.space_id());

			/* System table is always loaded. */
			ut_ad(fil_tablespace_exists_in_mem(tbl->m_space_id)
				== TRUE);

			fil_recreate_table(
				tbl->m_space_id,
				tbl->m_format_flags,
				tbl->m_tablespace_flags,
				tbl->m_tablename,
				*tbl, log_get_lsn(), redo_cache_entry);
		}

		redo_cache.push_back(redo_cache_entry);
	}

	/* Tables are already truncated using MLOG_FILE_TRUNCATE REDO log
	entry. Now assign new table-id to these tables so that purge
	action on these tables can be blocked. */
	for (ulint i = 0; i < redo_cache.size() && err != DB_ERROR; i++) {

		table_id_t      new_id;
		dict_hdr_get_new_id(&new_id, NULL, NULL, NULL, true);

		err = row_truncate_update_sys_tables_post_redo(
			redo_cache[i], new_id, TRUE);
		if (err != DB_SUCCESS) {
			break;
		}
	}

	for (ulint i = 0; i < srv_tables_to_truncate.size(); i++) {
		delete(srv_tables_to_truncate[i]);
	}
	srv_tables_to_truncate.clear();

	for (ulint i = 0; i < redo_cache.size(); i++) {
		delete(redo_cache[i]);
	}
	redo_cache.clear();

	return(err);
}

