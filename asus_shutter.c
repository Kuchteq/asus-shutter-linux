/* Copyright (c) 2010: Michal Kottman */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <linux/acpi.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Driver for asus shutter based on acpi-call");
MODULE_AUTHOR("Mariusz Jan Kuchta");
/* Uncomment the following line to enable debug messages */
/*
#define DEBUG
*/

#define BUFFER_SIZE 256
#define MAX_ACPI_ARGS 16

extern struct proc_dir_entry *acpi_root_dir;

static char result_buffer[BUFFER_SIZE];

static size_t get_avail_bytes(void)
{
	return BUFFER_SIZE - strlen(result_buffer);
}
static char *get_buffer_end(void)
{
	return result_buffer + strlen(result_buffer);
}

/** Appends the contents of an acpi_object to the result buffer
@param result   An acpi object holding result data
@returns        0 if the result could fully be saved, a higher value otherwise
*/
static int acpi_result_to_string(union acpi_object *result)
{
	if (result->type == ACPI_TYPE_INTEGER) {
		snprintf(get_buffer_end(), get_avail_bytes(), "0x%x",
			 (int)result->integer.value);
	} else if (result->type == ACPI_TYPE_STRING) {
		snprintf(get_buffer_end(), get_avail_bytes(), "\"%*s\"",
			 result->string.length, result->string.pointer);
	} else if (result->type == ACPI_TYPE_BUFFER) {
		int i;
		// do not store more than data if it does not fit. The first element is
		// just 4 chars, but there is also two bytes from the curly brackets
		int show_values = min((size_t)result->buffer.length,
				      get_avail_bytes() / 6);

		sprintf(get_buffer_end(), "{");
		for (i = 0; i < show_values; i++)
			sprintf(get_buffer_end(),
				i == 0 ? "0x%02x" : ", 0x%02x",
				result->buffer.pointer[i]);

		if (result->buffer.length > show_values) {
			// if data was truncated, show a trailing comma if there is space
			snprintf(get_buffer_end(), get_avail_bytes(), ",");
			return 1;
		} else {
			// in case show_values == 0, but the buffer is too small to hold
			// more values (i.e. the buffer cannot have anything more than "{")
			snprintf(get_buffer_end(), get_avail_bytes(), "}");
		}
	} else if (result->type == ACPI_TYPE_PACKAGE) {
		int i;
		sprintf(get_buffer_end(), "[");
		for (i = 0; i < result->package.count; i++) {
			if (i > 0)
				snprintf(get_buffer_end(), get_avail_bytes(),
					 ", ");

			// abort if there is no more space available
			if (!get_avail_bytes() ||
			    acpi_result_to_string(&result->package.elements[i]))
				return 1;
		}
		snprintf(get_buffer_end(), get_avail_bytes(), "]");
	} else {
		snprintf(get_buffer_end(), get_avail_bytes(),
			 "Object type 0x%x\n", result->type);
	}

	// return 0 if there are still bytes available, 1 otherwise
	return !get_avail_bytes();
}

/**
@param method   The full name of ACPI method to call
@param argc     The number of parameters
@param argv     A pre-allocated array of arguments of type acpi_object
*/
static void do_asus_shutter(const char *method, int argc, union acpi_object *argv)
{
	acpi_status status;
	acpi_handle handle;
	struct acpi_object_list arg;
	struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };

#ifdef DEBUG
	printk(KERN_INFO "asus_shutter: Calling %s\n", method);
#endif

	// get the handle of the method, must be a fully qualified path
	status = acpi_get_handle(NULL, (acpi_string)method, &handle);

	if (ACPI_FAILURE(status)) {
		snprintf(result_buffer, BUFFER_SIZE, "Error: %s",
			 acpi_format_exception(status));
		printk(KERN_ERR "asus_shutter: Cannot get handle: %s\n",
		       result_buffer);
		return;
	}

	// prepare parameters
	arg.count = argc;
	arg.pointer = argv;

	// call the method
	status = acpi_evaluate_object(handle, NULL, &arg, &buffer);
	if (ACPI_FAILURE(status)) {
		snprintf(result_buffer, BUFFER_SIZE, "Error: %s",
			 acpi_format_exception(status));
		printk(KERN_ERR "asus_shutter: Method call failed: %s\n",
		       result_buffer);
		return;
	}

	// reset the result buffer
	*result_buffer = '\0';
	acpi_result_to_string(buffer.pointer);
	kfree(buffer.pointer);

#ifdef DEBUG
	printk(KERN_INFO "asus_shutter: Call successful: %s\n", result_buffer);
#endif
}


/** procfs write callback. Called when writing into /proc/acpi/call.
*/

static ssize_t acpi_proc_write(struct file *file,
			       const char __user *user_buffer, size_t count,
			       loff_t *off)
{
	char input[2] = { '\0' };
	union acpi_object args[] = {
		{ .integer = { .type = ACPI_TYPE_INTEGER, .value = 0x0 } },
		{ .integer = { .type = ACPI_TYPE_INTEGER,
			       .value = 0x53564544 } },
		{ .integer = { .type = ACPI_TYPE_INTEGER,
			       // 0x00000001 removes the cover and all nulls trigger it again
			       .value = 0x0000000100060078 } }
	};
	int nargs = 3;
	char *method;

	if (count > sizeof(input) - 1) {
		// remember you can't have a , after message type!
		printk(KERN_ERR
		       "asus-shutter: Control request too long! (%lu)\n",
		       count);
		return -ENOSPC;
	}

	if (copy_from_user(input, user_buffer, count)) {
		return -EFAULT;
	}
	// If 1 then cover the camera, if 0 then remove the cover
	if (input[0] > 0x30) {
		args[2].integer.value ^= 0x0000000100000000;
	}
	input[count] = '\0';
	method = "\\_SB.ATKD.WMNB";

	printk(KERN_DEBUG "asus-shutter: Your input is: method is: %s\n",
	       method);
	do_asus_shutter(method, nargs, args);
	return count;
}

/** procfs 'call' read callback. Called when reading the content of /proc/acpi/call.
Returns the last call status:
- "not called" when no call was previously issued
- "failed" if the call failed
- "ok" if the call succeeded
*/
static ssize_t acpi_proc_read(struct file *filp, char __user *buff,
			      size_t count, loff_t *off)
{
	ssize_t ret;
	int len = strlen(result_buffer);

	// output the current result buffer
	ret = simple_read_from_buffer(buff, count, off, result_buffer, len + 1);

	// initialize the result buffer for later
	strcpy(result_buffer, "not called");

	return ret;
}

const struct proc_ops proc_acpi_operations = {
	.proc_read = acpi_proc_read,
	.proc_write = acpi_proc_write,
};

/** module initialization function */
static int __init init_asus_shutter(void)
{
	struct proc_dir_entry *acpi_entry = proc_create(
		"asus-shutter", 0660, acpi_root_dir, &proc_acpi_operations);
	strcpy(result_buffer, "not called");

	if (acpi_entry == NULL) {
		printk(KERN_ERR "asus-shutter: Couldn't create proc entry\n");
		return -ENOMEM;
	}
	printk(KERN_INFO "asus-shutter: Module loaded successfully\n");

	return 0;
}

static void __exit unload_asus_shutter(void)
{
	remove_proc_entry("asus-shutter", acpi_root_dir);
	printk(KERN_INFO "asus-shutter: Module unloaded successfully\n");
}

module_init(init_asus_shutter);
module_exit(unload_asus_shutter);
