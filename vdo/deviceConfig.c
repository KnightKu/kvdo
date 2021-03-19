/**
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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/deviceConfig.c#34 $
 */

#include "deviceConfig.h"

#include <linux/device-mapper.h>

#include "logger.h"
#include "memoryAlloc.h"
#include "stringUtils.h"

#include "kernelLayer.h"
#include "vdoStringUtils.h"

#include "constants.h"

enum {
	// If we bump this, update the arrays below
	TABLE_VERSION = 4,
	// Limits used when parsing thread-count config spec strings
	BIO_ROTATION_INTERVAL_LIMIT = 1024,
	LOGICAL_THREAD_COUNT_LIMIT = 60,
	PHYSICAL_THREAD_COUNT_LIMIT = 16,
	THREAD_COUNT_LIMIT = 100,
	// XXX The bio-submission queue configuration defaults are temporarily
	// still being defined here until the new runtime-based thread
	// configuration has been fully implemented for managed VDO devices.

	// How many bio submission work queues to use
	DEFAULT_NUM_BIO_SUBMIT_QUEUES = 4,
	// How often to rotate between bio submission work queues
	DEFAULT_BIO_SUBMIT_QUEUE_ROTATE_INTERVAL = 64,
};

// arrays for handling different table versions
static const uint8_t REQUIRED_ARGC[] = { 10, 12, 9, 7, 6 };
// pool name no longer used. only here for verification of older versions
static const uint8_t POOL_NAME_ARG_INDEX[] = { 8, 10, 8 };

/**
 * Decide the version number from argv.
 *
 * @param [in]  argc         The number of table values
 * @param [in]  argv         The array of table values
 * @param [out] error_ptr    A pointer to return a error string in
 * @param [out] version_ptr  A pointer to return the version
 *
 * @return VDO_SUCCESS or an error code
 **/
static int get_version_number(int argc,
			      char **argv,
			      char **error_ptr,
			      unsigned int *version_ptr)
{
	// version, if it exists, is in a form of V<n>
	if (sscanf(argv[0], "V%u", version_ptr) == 1) {
		if (*version_ptr < 1 || *version_ptr > TABLE_VERSION) {
			*error_ptr = "Unknown version number detected";
			return VDO_BAD_CONFIGURATION;
		}
	} else {
		// V0 actually has no version number in the table string
		*version_ptr = 0;
	}

	// V0 and V1 have no optional parameters. There will always be
	// a parameter for thread config, even if it's a "." to show
	// it's an empty list.
	if (*version_ptr <= 1) {
		if (argc != REQUIRED_ARGC[*version_ptr]) {
			*error_ptr =
				"Incorrect number of arguments for version";
			return VDO_BAD_CONFIGURATION;
		}
	} else if (argc < REQUIRED_ARGC[*version_ptr]) {
		*error_ptr = "Incorrect number of arguments for version";
		return VDO_BAD_CONFIGURATION;
	}

	if (*version_ptr != TABLE_VERSION) {
		log_warning("Detected version mismatch between kernel module and tools kernel: %d, tool: %d",
			    TABLE_VERSION,
			    *version_ptr);
		log_warning("Please consider upgrading management tools to match kernel.");
	}
	return VDO_SUCCESS;
}

/**
 * Parse a two-valued option into a bool.
 *
 * @param [in]  bool_str   The string value to convert to a bool
 * @param [in]  true_str   The string value which should be converted to true
 * @param [in]  false_str  The string value which should be converted to false
 * @param [out] bool_ptr   A pointer to return the bool value in
 *
 * @return VDO_SUCCESS or an error if bool_str is neither true_str
 *                        nor false_str
 **/
static inline int __must_check
parse_bool(const char *bool_str,
	   const char *true_str,
	   const char *false_str,
	   bool *bool_ptr)
{
	bool value = false;

	if (strcmp(bool_str, true_str) == 0) {
		value = true;
	} else if (strcmp(bool_str, false_str) == 0) {
		value = false;
	} else {
		return VDO_BAD_CONFIGURATION;
	}

	*bool_ptr = value;
	return VDO_SUCCESS;
}

/**
 * Process one component of a thread parameter configuration string and
 * update the configuration data structure.
 *
 * If the thread count requested is invalid, a message is logged and
 * -EINVAL returned. If the thread name is unknown, a message is logged
 * but no error is returned.
 *
 * @param thread_param_type  The type of thread specified
 * @param count              The thread count requested
 * @param config             The configuration data structure to update
 *
 * @return VDO_SUCCESS or -EINVAL
 **/
static int process_one_thread_config_spec(const char *thread_param_type,
					  unsigned int count,
					  struct thread_count_config *config)
{
	// Handle limited thread parameters
	if (strcmp(thread_param_type, "bioRotationInterval") == 0) {
		if (count == 0) {
			uds_log_error("thread config string error:  'bioRotationInterval' of at least 1 is required");
			return -EINVAL;
		} else if (count > BIO_ROTATION_INTERVAL_LIMIT) {
			uds_log_error("thread config string error: 'bioRotationInterval' cannot be higher than %d",
				      BIO_ROTATION_INTERVAL_LIMIT);
			return -EINVAL;
		}
		config->bio_rotation_interval = count;
		return VDO_SUCCESS;
	} else if (strcmp(thread_param_type, "logical") == 0) {
		if (count > LOGICAL_THREAD_COUNT_LIMIT) {
			uds_log_error("thread config string error: at most %d 'logical' threads are allowed",
				      LOGICAL_THREAD_COUNT_LIMIT);
			return -EINVAL;
		}
		config->logical_zones = count;
		return VDO_SUCCESS;
	} else if (strcmp(thread_param_type, "physical") == 0) {
		if (count > PHYSICAL_THREAD_COUNT_LIMIT) {
			uds_log_error("thread config string error: at most %d 'physical' threads are allowed",
				      PHYSICAL_THREAD_COUNT_LIMIT);
			return -EINVAL;
		}
		config->physical_zones = count;
		return VDO_SUCCESS;
	} else {
		// Handle other thread count parameters
		if (count > THREAD_COUNT_LIMIT) {
			uds_log_error("thread config string error: at most %d '%s' threads are allowed",
				      THREAD_COUNT_LIMIT,
				      thread_param_type);
			return -EINVAL;
		}

		if (strcmp(thread_param_type, "hash") == 0) {
			config->hash_zones = count;
			return VDO_SUCCESS;
		} else if (strcmp(thread_param_type, "cpu") == 0) {
			if (count == 0) {
				uds_log_error("thread config string error: at least one 'cpu' thread required");
				return -EINVAL;
			}
			config->cpu_threads = count;
			return VDO_SUCCESS;
		} else if (strcmp(thread_param_type, "ack") == 0) {
			config->bio_ack_threads = count;
			return VDO_SUCCESS;
		} else if (strcmp(thread_param_type, "bio") == 0) {
			if (count == 0) {
				uds_log_error("thread config string error: at least one 'bio' thread required");
				return -EINVAL;
			}
			config->bio_threads = count;
			return VDO_SUCCESS;
		}
	}

	// Don't fail, just log. This will handle version mismatches between
	// user mode tools and kernel.
	log_info("unknown thread parameter type \"%s\"", thread_param_type);
	return VDO_SUCCESS;
}

/**
 * Parse one component of a thread parameter configuration string and
 * update the configuration data structure.
 *
 * @param spec    The thread parameter specification string
 * @param config  The configuration data to be updated
 **/
static int parse_one_thread_config_spec(const char *spec,
					struct thread_count_config *config)
{
	unsigned int count;
	char **fields;
	int result = split_string(spec, '=', &fields);

	if (result != UDS_SUCCESS) {
		return result;
	}
	if ((fields[0] == NULL) || (fields[1] == NULL) || (fields[2] != NULL)) {
		uds_log_error("thread config string error: expected thread parameter assignment, saw \"%s\"",
			      spec);
		free_string_array(fields);
		return -EINVAL;
	}

	result = string_to_uint(fields[1], &count);
	if (result != UDS_SUCCESS) {
		uds_log_error("thread config string error: integer value needed, found \"%s\"",
			      fields[1]);
		free_string_array(fields);
		return result;
	}

	result = process_one_thread_config_spec(fields[0], count, config);
	free_string_array(fields);
	return result;
}

/**
 * Parse the configuration string passed and update the specified
 * counts and other parameters of various types of threads to be created.
 *
 * The configuration string should contain one or more comma-separated specs
 * of the form "typename=number"; the supported type names are "cpu", "ack",
 * "bio", "bioRotationInterval", "logical", "physical", and "hash".
 *
 * If an error occurs during parsing of a single key/value pair, we deem
 * it serious enough to stop further parsing.
 *
 * This function can't set the "reason" value the caller wants to pass
 * back, because we'd want to format it to say which field was
 * invalid, and we can't allocate the "reason" strings dynamically. So
 * if an error occurs, we'll log the details and pass back an error.
 *
 * @param string  Thread parameter configuration string
 * @param config  The thread configuration data to update
 *
 * @return VDO_SUCCESS or -EINVAL or -ENOMEM
 **/
static int parse_thread_config_string(const char *string,
				      struct thread_count_config *config)
{
	int result = VDO_SUCCESS;

	char **specs;

	if (strcmp(".", string) != 0) {
		unsigned int i;
		result = split_string(string, ',', &specs);
		if (result != UDS_SUCCESS) {
			return result;
		}

		for (i = 0; specs[i] != NULL; i++) {
			result = parse_one_thread_config_spec(specs[i], config);
			if (result != VDO_SUCCESS) {
				break;
			}
		}
		free_string_array(specs);
	}
	return result;
}

/**
 * Process one component of an optional parameter string and update
 * the configuration data structure.
 *
 * If the value requested is invalid, a message is logged and -EINVAL
 * returned. If the key is unknown, a message is logged but no error
 * is returned.
 *
 * @param key     The optional parameter key name
 * @param value   The optional parameter value
 * @param config  The configuration data structure to update
 *
 * @return VDO_SUCCESS or -EINVAL
 **/
static int process_one_key_value_pair(const char *key,
				      unsigned int value,
				      struct device_config *config)
{
	// Non thread optional parameters
	if (strcmp(key, "maxDiscard") == 0) {
		if (value == 0) {
			uds_log_error("optional parameter error: at least one max discard block required");
			return -EINVAL;
		}
		// Max discard sectors in blkdev_issue_discard is UINT_MAX >> 9
		if (value > (UINT_MAX / VDO_BLOCK_SIZE)) {
			uds_log_error("optional parameter error: at most %d max discard  blocks are allowed",
				      UINT_MAX / VDO_BLOCK_SIZE);
			return -EINVAL;
		}
		config->max_discard_blocks = value;
		return VDO_SUCCESS;
	}
	// Handles unknown key names
	return process_one_thread_config_spec(key, value,
					      &config->thread_counts);
}

/**
 * Parse one key/value pair and update the configuration
 * data structure.
 *
 * @param key     The optional key name
 * @param value   The optional value
 * @param config  The configuration data to be updated
 *
 * @return VDO_SUCCESS or error
 **/
static int parse_one_key_value_pair(const char *key,
				    const char *value,
				    struct device_config *config)
{
	unsigned int count;
	int result;

	if (strcmp(key, "deduplication") == 0) {
		return parse_bool(value, "on", "off", &config->deduplication);
	}

	// The remaining arguments must have integral values.
	result = string_to_uint(value, &count);
	if (result != UDS_SUCCESS) {
		uds_log_error("optional config string error: integer value needed, found \"%s\"",
			      value);
		return result;
	}
	return process_one_key_value_pair(key, count, config);
}

/**
 * Parse all key/value pairs from a list of arguments.
 *
 * If an error occurs during parsing of a single key/value pair, we deem
 * it serious enough to stop further parsing.
 *
 * This function can't set the "reason" value the caller wants to pass
 * back, because we'd want to format it to say which field was
 * invalid, and we can't allocate the "reason" strings dynamically. So
 * if an error occurs, we'll log the details and return the error.
 *
 * @param argc    The total number of arguments in list
 * @param argv    The list of key/value pairs
 * @param config  The device configuration data to update
 *
 * @return VDO_SUCCESS or error
 **/
static int parse_key_value_pairs(int argc,
				 char **argv,
				 struct device_config *config)
{
	int result = VDO_SUCCESS;

	while (argc) {
		result = parse_one_key_value_pair(argv[0], argv[1], config);
		if (result != VDO_SUCCESS) {
			break;
		}

		argc -= 2;
		argv += 2;
	}

	return result;
}

/**
 * Parse the configuration string passed in for optional arguments.
 *
 * For V0/V1 configurations, there will only be one optional parameter;
 * the thread configuration. The configuration string should contain
 * one or more comma-separated specs of the form "typename=number"; the
 * supported type names are "cpu", "ack", "bio", "bioRotationInterval",
 * "logical", "physical", and "hash".
 *
 * For V2 configurations and beyond, there could be any number of
 * arguments. They should contain one or more key/value pairs
 * separated by a space.
 *
 * @param arg_set    The structure holding the arguments to parse
 * @param error_ptr  Pointer to a buffer to hold the error string
 * @param config     Pointer to device configuration data to update
 *
 * @return VDO_SUCCESS or error
 */
int parse_optional_arguments(struct dm_arg_set *arg_set,
			     char **error_ptr,
			     struct device_config *config)
{
	int result = VDO_SUCCESS;

	if (config->version == 0 || config->version == 1) {
		result = parse_thread_config_string(arg_set->argv[0],
						    &config->thread_counts);
		if (result != VDO_SUCCESS) {
			*error_ptr = "Invalid thread-count configuration";
			return VDO_BAD_CONFIGURATION;
		}
	} else {
		if ((arg_set->argc % 2) != 0) {
			*error_ptr = "Odd number of optional arguments given but they should be <key> <value> pairs";
			return VDO_BAD_CONFIGURATION;
		}
		result = parse_key_value_pairs(arg_set->argc,
					       arg_set->argv,
					       config);
		if (result != VDO_SUCCESS) {
			*error_ptr = "Invalid optional argument configuration";
			return VDO_BAD_CONFIGURATION;
		}
	}
	return result;
}

/**
 * Handle a parsing error.
 *
 * @param config_ptr  A pointer to the config to free
 * @param error_ptr   A place to store a constant string about the error
 * @param error_str   A constant string to store in error_ptr
 **/
static void handle_parse_error(struct device_config **config_ptr,
			       char **error_ptr,
			       char *error_str)
{
	free_device_config(config_ptr);
	*error_ptr = error_str;
}

/**********************************************************************/
int parse_device_config(int argc,
			char **argv,
			struct dm_target *ti,
			struct device_config **config_ptr)
{
	bool enable_512e;
	struct dm_arg_set arg_set;

	char **error_ptr = &ti->error;
	struct device_config *config = NULL;
	int result =
		ALLOCATE(1, struct device_config, "device_config", &config);
	if (result != VDO_SUCCESS) {
		handle_parse_error(&config,
				   error_ptr,
				   "Could not allocate config structure");
		return VDO_BAD_CONFIGURATION;
	}

	config->owning_target = ti;
	INIT_LIST_HEAD(&config->config_list);

	// Save the original string.
	result = join_strings(argv, argc, ' ', &config->original_string);
	if (result != VDO_SUCCESS) {
		handle_parse_error(
			&config, error_ptr, "Could not populate string");
		return VDO_BAD_CONFIGURATION;
	}

	// Set defaults.
	//
	// XXX Defaults for bio_threads and bio_rotation_interval are currently
	// defined using the old configuration scheme of constants.  These
	// values are relied upon for performance testing on MGH machines
	// currently. This should be replaced with the normally used testing
	// defaults being defined in the file-based thread-configuration
	// settings.  The values used as defaults internally should really be
	// those needed for VDO in its default shipped-product state.
	config->thread_counts = (struct thread_count_config) {
		.bio_ack_threads = 1,
		.bio_threads = DEFAULT_NUM_BIO_SUBMIT_QUEUES,
		.bio_rotation_interval =
			DEFAULT_BIO_SUBMIT_QUEUE_ROTATE_INTERVAL,
		.cpu_threads = 1,
		.logical_zones = 0,
		.physical_zones = 0,
		.hash_zones = 0,
	};
	config->max_discard_blocks = 1;
	config->deduplication = true;

	arg_set.argc = argc;
	arg_set.argv = argv;

	result = get_version_number(argc, argv, error_ptr, &config->version);
	if (result != VDO_SUCCESS) {
		// get_version_number sets error_ptr itself.
		handle_parse_error(&config, error_ptr, *error_ptr);
		return result;
	}
	// Move the arg pointer forward only if the argument was there.
	if (config->version >= 1) {
		dm_shift_arg(&arg_set);
	}

	result = duplicate_string(dm_shift_arg(&arg_set),
				  "parent device name",
				  &config->parent_device_name);
	if (result != VDO_SUCCESS) {
		handle_parse_error(&config,
				   error_ptr,
				   "Could not copy parent device name");
		return VDO_BAD_CONFIGURATION;
	}

	// Get the physical blocks, if known.
	if (config->version >= 1) {
		result = kstrtoull(dm_shift_arg(&arg_set),
				   10,
				   &config->physical_blocks);
		if (result != VDO_SUCCESS) {
			handle_parse_error(&config,
					   error_ptr,
					   "Invalid physical block count");
			return VDO_BAD_CONFIGURATION;
		}
	}

	// Get the logical block size and validate
	result = parse_bool(dm_shift_arg(&arg_set),
			    "512",
			    "4096",
			    &enable_512e);
	if (result != VDO_SUCCESS) {
		handle_parse_error(&config,
				   error_ptr,
				   "Invalid logical block size");
		return VDO_BAD_CONFIGURATION;
	}
	config->logical_block_size = (enable_512e ? 512 : 4096);

	// Skip past the two no longer used read cache options.
	if (config->version <= 1) {
		dm_consume_args(&arg_set, 2);
	}

	// Get the page cache size.
	result = string_to_uint(dm_shift_arg(&arg_set), &config->cache_size);
	if (result != VDO_SUCCESS) {
		handle_parse_error(&config,
				   error_ptr,
				   "Invalid block map page cache size");
		return VDO_BAD_CONFIGURATION;
	}

	// Get the block map era length.
	result = string_to_uint(dm_shift_arg(&arg_set),
				&config->block_map_maximum_age);
	if (result != VDO_SUCCESS) {
		handle_parse_error(&config,
				   error_ptr,
				   "Invalid block map maximum age");
		return VDO_BAD_CONFIGURATION;
	}

	// Skip past the no longer used MD RAID5 optimization mode
	if (config->version <= 2) {
		dm_consume_args(&arg_set, 1);
	}

	// Skip past the no longer used write policy setting
	if (config->version <= 3) {
		dm_consume_args(&arg_set, 1);
	}

	// Skip past the no longer used pool name for older table lines
	if (config->version <= 2) {
		// Make sure the enum to get the pool name from argv directly
		// is still in sync with the parsing of the table line.
		if (&arg_set.argv[0] !=
		    &argv[POOL_NAME_ARG_INDEX[config->version]]) {
			handle_parse_error(&config,
					   error_ptr,
					   "Pool name not in expected location");
			return VDO_BAD_CONFIGURATION;
		}
		dm_shift_arg(&arg_set);
	}

	// Get the optional arguments and validate.
	result = parse_optional_arguments(&arg_set, error_ptr, config);
	if (result != VDO_SUCCESS) {
		// parse_optional_arguments sets error_ptr itself.
		handle_parse_error(&config, error_ptr, *error_ptr);
		return result;
	}

	/*
	 * Logical, physical, and hash zone counts can all be zero; then we get
	 * one thread doing everything, our older configuration. If any zone
	 * count is non-zero, the others must be as well.
	 */
	if (((config->thread_counts.logical_zones == 0) !=
	     (config->thread_counts.physical_zones == 0)) ||
	    ((config->thread_counts.physical_zones == 0) !=
	     (config->thread_counts.hash_zones == 0))) {
		handle_parse_error(&config,
				   error_ptr,
				   "Logical, physical, and hash zones counts must all be zero or all non-zero");
		return VDO_BAD_CONFIGURATION;
	}

	result = dm_get_device(ti,
			       config->parent_device_name,
			       dm_table_get_mode(ti->table),
			       &config->owned_device);
	if (result != 0) {
		uds_log_error("couldn't open device \"%s\": error %d",
			      config->parent_device_name,
			      result);
		handle_parse_error(&config,
				   error_ptr,
				   "Unable to open storage device");
		return VDO_BAD_CONFIGURATION;
	}

	if (config->version == 0) {
		uint64_t device_size =
			i_size_read(config->owned_device->bdev->bd_inode);

		config->physical_blocks = device_size / VDO_BLOCK_SIZE;
	}

	*config_ptr = config;
	return result;
}

/**********************************************************************/
void free_device_config(struct device_config **config_ptr)
{
	struct device_config *config;
	if (config_ptr == NULL) {
		return;
	}

	config = *config_ptr;

	if (config == NULL) {
		*config_ptr = NULL;
		return;
	}

	if (config->owned_device != NULL) {
		dm_put_device(config->owning_target, config->owned_device);
	}

	FREE(config->parent_device_name);
	FREE(config->original_string);

	// Reduce the chance a use-after-free (as in BZ 1669960) happens to work.
	memset(config, 0, sizeof(*config));

	FREE(config);
	*config_ptr = NULL;
}

/**********************************************************************/
void set_device_config_layer(struct device_config *config,
			     struct kernel_layer *layer)
{
	list_del_init(&config->config_list);
	if (layer != NULL) {
		list_add_tail(&config->config_list,
			      &layer->device_config_list);
	}
	config->layer = layer;
}
