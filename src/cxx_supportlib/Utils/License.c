/*
 *  Phusion Passenger - http://www.modrails.com/
 *  Copyright (c) 2012-2016 Phusion Holding B.V.
 *
 *  "Passenger", "Phusion Passenger" and "Union Station" are registered
 *  trademarks of Phusion Holding B.V.
 *
 *  See LICENSE file for license information.
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "MD5.h"
#include "License.h"

#ifdef __cplusplus
namespace Passenger {
#endif

#define MAX_LICENSE_LINES 30
#define LICENSE_SECRET "An error occurred while fetching this page. Please contact an administrator if this problem persists."
/* N.B. there is a legacy field named "Valid until:" that used to signify the fastspring license expiration and might still be present
 * in old certificates, so "Expires after:" was deliberately chosen to avoid that legacy.
 */
#define EXPIRES_AFTER_KEY "Expires after:"

/* Workaround to shut up compiler warnings about return values */
#define IGNORE_RETVAL(val) \
	do { \
		if (val == 0) { \
			printf("\n"); \
		} \
	} while (0)

char *licenseKey = (char *) 0;

static md5_byte_t
hexNibbleToByte(char hexNibble) {
	if (hexNibble >= '0' && hexNibble <= '9') {
		return hexNibble - '0';
	} else {
		return hexNibble - 'A' + 10;
	}
}

/*
 * Returns pointer to EXPIRES_AFTER_KEY date value (yyyy-mm-dd) within licenseKey, or NULL if not there.
 */
const char *
findExpiresAfterDate(const char *licenseKey) {
	const char *expiresAfterChars = strstr(licenseKey, EXPIRES_AFTER_KEY);
	if (expiresAfterChars == NULL) {
		return NULL;
	}

	expiresAfterChars += sizeof(EXPIRES_AFTER_KEY);
	while (*expiresAfterChars == ' ') {
		expiresAfterChars++;
	}

	return expiresAfterChars;
}

/*
 * Returns 0 if expired, 1 otherwise
 */
int
compareDates(const char *expiresAfterChars, struct tm checkDate) {
	// A license without Expires after is valid forever.
	if (expiresAfterChars == NULL) {
		return 1;
	}

	// Otherwise, valid until the date specified in the license is in the past
	char checkDateChars[20];
	snprintf(checkDateChars, sizeof(checkDateChars), "%d-%02d-%02d", checkDate.tm_year + 1900, checkDate.tm_mon + 1, checkDate.tm_mday);

	if (strcmp(expiresAfterChars, checkDateChars) >= 0) {
		return 1;
	}

	return 0;
}

void
passenger_enterprise_license_init() {
	/* Because of bugs on certain operating systems, we cannot assume that
	 * `licenseKey` is NULL before initialization. This init function
	 * initializes global variables. See email by Jason Rust,
	 * "Not loading after installing 4.0.7", July 5 2013.
	 */
	licenseKey = NULL;
}

static FILE *
open_license_file() {
	const char *licenseData = getenv("PASSENGER_ENTERPRISE_LICENSE_DATA");
	if (licenseData != NULL && *licenseData != '\0') {
		char path[PATH_MAX] = "/tmp/passenger.XXXXXXXX";
		int fd = mkstemp(path);
		FILE *f;

		if (fd == -1) {
			int e = errno;
			fprintf(stderr, "Error: Phusion Passenger Enterprise license detected "
				"in environment variable PASSENGER_ENTERPRISE_LICENSE_DATA, "
				"but unable to create a temporary file: %s (errno=%d)\n",
				strerror(e), e);
			return NULL;
		}

		unlink(path);
		f = fdopen(fd, "r+");
		if (f != NULL) {
			size_t len = strlen(licenseData);
			size_t ret;

			ret = fwrite(licenseData, 1, len, f);
			IGNORE_RETVAL(ret);
			if (len > 0 && licenseData[len - 1] != '\n') {
				ret = fwrite("\n", 1, 1, f);
				IGNORE_RETVAL(ret);
			}
			ret = fseek(f, 0, SEEK_SET);
			IGNORE_RETVAL(ret);
		}
		return f;
	} else {
		return fopen("/etc/passenger-enterprise-license", "r");
	}
}

char *
passenger_enterprise_license_check() {
	FILE *f;
	char *lines[MAX_LICENSE_LINES];
	char line[1024];
	unsigned int count = 0, i;
	char *message = NULL;
	size_t len, totalSize = 0;
	struct md5_state_s md5;
	md5_byte_t digest[MD5_SIZE], readDigest[MD5_SIZE], *readDigestCursor;
	const char *data, *dataEnd;
	char *dataEnd2;
	time_t t = time(NULL);
	struct tm dateTimeNow;
	const char *expiresAfter;

	if (licenseKey != NULL) {
		return strdup("Phusion Passenger Enterprise license key already checked.");
	}

	f = open_license_file();
	if (f == NULL) {
		return strdup("Could not open the Phusion Passenger Enterprise license file. "
			"Please check whether it's installed correctly and whether it's world-readable.\n"
			APPEAL_MESSAGE);
	}

	// Read all lines in the license key file and store them in a string array.
	while (1) {
		if (fgets(line, sizeof(line), f) == NULL) {
			if (ferror(f)) {
				message = strdup(
					"An I/O error occurred while reading the Phusion Passenger Enterprise license file.\n"
					APPEAL_MESSAGE);
				goto finish;
			} else {
				break;
			}
		}

		len = strlen(line);
		if (len == 0 || line[len - 1] != '\n' || count >= MAX_LICENSE_LINES) {
			message = strdup("The Phusion Passenger Enterprise license file appears to be corrupted. Please reinstall it.\n"
				APPEAL_MESSAGE);
			goto finish;
		}

		lines[count] = strdup(line);
		count++;
		totalSize += len;
	}

	if (count == 0) {
		message = strdup("The Phusion Passenger Enterprise license file appears to be corrupted. Please reinstall it.\n"
			APPEAL_MESSAGE);
		goto finish;
	}

	// Calculate MD5 of the license key file.
	md5_init(&md5);
	for (i = 0; i < count - 1; i++) {
		md5_append(&md5, (const md5_byte_t *) lines[i], strlen(lines[i]));
	}
	md5_append(&md5, (const md5_byte_t *) LICENSE_SECRET, sizeof(LICENSE_SECRET) - 1);
	md5_finish(&md5, digest);

	// Read the last line of the license key file as binary MD5 data.
	readDigestCursor = readDigest;
	data = lines[count - 1];
	dataEnd = data + strlen(data);
	while (data < dataEnd && *data != '\n' && readDigestCursor < readDigest + MD5_SIZE) {
		while (*data == ' ') {
			data++;
		}
		*readDigestCursor = (hexNibbleToByte(data[0]) << 4) | hexNibbleToByte(data[1]);
		readDigestCursor++;
		data += 2;
	}

	// Validate MD5 checksum.
	if (memcmp(digest, readDigest, MD5_SIZE) != 0) {
		message = strdup("The Phusion Passenger Enterprise license file is invalid.\n"
			APPEAL_MESSAGE);
		goto finish;
	}

	// Create null-terminated buffer containing license key file data,
	// excluding last line.
	licenseKey = (char *) malloc(totalSize + 1);
	dataEnd2 = licenseKey;
	for (i = 0; i < count - 1; i++) {
		len = strlen(lines[i]);
		memcpy(dataEnd2, lines[i], len);
		dataEnd2 += len;
	}
	*dataEnd2 = '\0';

	// If there is a validity limit, check it
	expiresAfter = findExpiresAfterDate(licenseKey);
	dateTimeNow = *localtime(&t);
	if (compareDates(expiresAfter, dateTimeNow) == 0) {
		free(licenseKey);
		licenseKey = NULL;
		int messageLen = sizeof("The Phusion Passenger Enterprise license file is invalid: expired since .\n") + strlen(expiresAfter) + sizeof(EXPIRED_APPEAL_MESSAGE) + 1;
		message = (char *) malloc(messageLen);
		snprintf(message, messageLen, "The Phusion Passenger Enterprise license file is invalid: expired since %s.\n%s", expiresAfter, EXPIRED_APPEAL_MESSAGE);
		goto finish;
	}

	finish:
	fclose(f);
	for (i = 0; i < count; i++) {
		free(lines[i]);
	}
	return message;
}

static int
passenger_enterprise_on_cloud_license() {
	return strstr(licenseKey, "Cloud license") != NULL;
}

static int
passenger_enterprise_on_heroku_license() {
	return strstr(licenseKey, "Heroku license") != NULL;
}

int
passenger_enterprise_should_track_usage() {
	return passenger_enterprise_on_cloud_license()
		|| passenger_enterprise_on_heroku_license();
}

#ifdef __cplusplus
} // namespace Passenger
#endif
