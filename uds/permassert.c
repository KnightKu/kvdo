/*
 * Copyright Red Hat
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. 
 *
 * $Id: //eng/uds-releases/krusty/src/uds/permassert.c#13 $
 */

#include "permassert.h"

#include "errors.h"
#include "logger.h"


/**********************************************************************/
int uds_assertion_failed(const char *expression_string,
			 int code,
			 const char *module_name,
			 const char *file_name,
			 int line_number,
			 const char *format,
			 ...)
{
	va_list args;
	va_start(args, format);

	uds_log_embedded_message(UDS_LOG_ERR,
				 module_name,
				 "assertion \"",
				 format,
				 args,
				 "\" (%s) failed at %s:%d",
				 expression_string,
				 file_name,
				 line_number);
	uds_log_backtrace(UDS_LOG_ERR);


	va_end(args);

	return code;
}
