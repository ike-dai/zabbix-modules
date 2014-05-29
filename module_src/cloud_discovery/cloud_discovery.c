/*
** Copyright (C) 2014 Daisuke Ikeda
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "sysinc.h"
#include "module.h"
#include "zbxjson.h"
#include "ipc.h"
#include "memalloc.h"
#include "log.h"
#include "zbxalgo.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <libdeltacloud/libdeltacloud.h>
//#include "memwatch.h"

#define ZBX_IPC_CLOUD_ID 'c'
#define NAME_MACRO "{#INSTANCE.NAME}"
#define ID_MACRO "{#INSTANCE.ID}"
#define PUBLIC_ADDR_MACRO "{#INSTANCE.PUBLIC_ADDR}"
#define PRIVATE_ADDR_MACRO "{#INSTANCE.PRIVATE_ADDR}"
#define METRIC_NAME_MACRO "{#METRIC.NAME}"
#define METRIC_UNIT_MACRO "{#METRIC.UNIT}"
#define CONFIG_FILE "/usr/local/zabbix/2.1.7/etc/zabbix_agentd.conf"
#define MEM_SIZE 1048576
#define EXPIRE_TIME 60*60*24

/* the variable keeps timeout setting for item processing */
static int	item_timeout = 0;

int	zbx_module_cloud_instance_discovery(AGENT_REQUEST *request, AGENT_RESULT *result);
int	zbx_module_cloud_instance_info(AGENT_REQUEST *request, AGENT_RESULT *result);
int	zbx_module_cloud_metric_discovery(AGENT_REQUEST *request, AGENT_RESULT *result);
int	zbx_module_cloud_metric(AGENT_REQUEST *request, AGENT_RESULT *result);

static zbx_mem_info_t   *cloud_mem = NULL;

ZBX_MEM_FUNC_IMPL(__cloud, cloud_mem);

//////


typedef struct
{
	zbx_vector_ptr_t	services;
}
zbx_deltacloud_t;

typedef struct
{
	char    *url;
        char    *key;
        char    *secret;
        char    *driver;
        char    *provider;
        int	lastcheck;
        int	lastaccess;
        zbx_vector_ptr_t  instances;
}
zbx_deltacloud_service_t;

typedef struct deltacloud_metric_value zbx_deltacloud_metric_value_t;
/* Ref. struct deltacloud_metric_value
 *   char *unit
 *   char *minimum
 *   char *maximum
 *   char *samples
 *   char *average
 *   struct deltacloud_metric_value *next
 */

typedef struct
{
	char *href;
	char *name;
	zbx_vector_ptr_t values;
}
zbx_deltacloud_metric_t;
	
typedef struct deltacloud_hardware_profile zbx_deltacloud_hardware_profile_t;
/* Ref. struct deltacloud_hardware_profile
 *   char *href;
 *   char *id;
 *   char *name;
 *   struct deltacloud_property *properties;
 */

typedef struct
{
	char *href;
	char *id;
	char *name;
	char *owner_id;
	char *image_id;
	char *image_href;
	char *realm_id;
	char *realm_href;
	char *state;
	char *launch_time;
	zbx_deltacloud_hardware_profile_t *hwp;
	zbx_vector_ptr_t public_addresses;
	zbx_vector_ptr_t private_addresses;
        zbx_vector_ptr_t metrics;
}
zbx_deltacloud_instance_t;

typedef struct deltacloud_address zbx_deltacloud_address_t;
/* Ref. struct deltacloud_address
 *   char *address;
 *   struct deltacloud_address *next;
 */

static void     cloud_service_shared_free(zbx_deltacloud_service_t *service);
static void	cloud_instance_shared_free(zbx_deltacloud_instance_t *instance);
static void	cloud_metric_shared_free(zbx_deltacloud_metric_t *metric);
static void	cloud_metric_value_shared_free(zbx_deltacloud_metric_value_t *value);

static zbx_deltacloud_t	*deltacloud = NULL; 

#define CLOUD_VECTOR_CREATE(ref, type) zbx_vector_##type##_create_ext(ref, __cloud_mem_malloc_func, __cloud_mem_realloc_func, __cloud_mem_free_func)

///////

static ZBX_METRIC keys[] =
/*      KEY                     FLAG		FUNCTION        	TEST PARAMETERS */
{
	{"cloud.instance.discovery",	CF_HAVEPARAMS,	zbx_module_cloud_instance_discovery,"http://hostname/api,ABC1223DE,ZDADQWQ2133,ec2,ap-northeast-1"},
	{"cloud.instance.info",	CF_HAVEPARAMS,	zbx_module_cloud_instance_info,"http://hostname/api,ABC1223DE,ZDADQWQ2133,ec2,ap-northeast-1,instance_id,element"},
	{"cloud.metric.discovery",	CF_HAVEPARAMS,	zbx_module_cloud_metric_discovery,"http://hostname/api,ABC1223DE,ZDADQWQ2133,ec2,ap-northeast-1,instance_id"},
	{"cloud.metric",	CF_HAVEPARAMS,	zbx_module_cloud_metric,"http://hostname/api,ABC1223DE,ZDADQWQ2133,ec2,ap-northeast-1,instance_id,DiskReadOps,average"},
	{NULL}
};

/******************************************************************************
 *                                                                            *
 * Function: zbx_module_api_version                                           *
 *                                                                            *
 * Purpose: returns version number of the module interface                    *
 *                                                                            *
 * Return value: ZBX_MODULE_API_VERSION_ONE - the only version supported by   *
 *               Zabbix currently                                             *
 *                                                                            *
 ******************************************************************************/
int	zbx_module_api_version()
{
	return ZBX_MODULE_API_VERSION_ONE;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_module_item_timeout                                          *
 *                                                                            *
 * Purpose: set timeout value for processing of items                         *
 *                                                                            *
 * Parameters: timeout - timeout in seconds, 0 - no timeout set               *
 *                                                                            *
 ******************************************************************************/
void	zbx_module_item_timeout(int timeout)
{
	item_timeout = timeout;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_module_item_list                                             *
 *                                                                            *
 * Purpose: returns list of item keys supported by the module                 *
 *                                                                            *
 * Return value: list of item keys                                            *
 *                                                                            *
 ******************************************************************************/
ZBX_METRIC	*zbx_module_item_list()
{
	return keys;
}


static char	*cloud_shared_strdup(const char *source)
{
	char	*ptr = NULL;
	size_t	len;

	if (NULL != source)
	{
		len = strlen(source) + 1;
		ptr = __cloud_mem_malloc_func(NULL, len);
		memcpy(ptr, source, len);
	}

	return ptr;
}

static zbx_deltacloud_hardware_profile_t *cloud_hardware_profile_shared_dup(const struct deltacloud_hardware_profile *src)
{
	zbx_deltacloud_hardware_profile_t	*hardware_profile;
	hardware_profile = __cloud_mem_malloc_func(NULL, sizeof(zbx_deltacloud_hardware_profile_t));
	
	hardware_profile->href = cloud_shared_strdup(src->href);
	hardware_profile->id = cloud_shared_strdup(src->id);
	hardware_profile->name = cloud_shared_strdup(src->name);
	return hardware_profile;
}
	

static zbx_deltacloud_service_t	*zbx_deltacloud_get_service(const char* url, const char* key, const char* secret, const char* driver, const char* provider)
{
	int i;
	zbx_deltacloud_service_t	*service = NULL;

	if (NULL == deltacloud)
	{
		zabbix_log(LOG_LEVEL_ERR, "---Not initialized shared memory---");
		return NULL;
	}

	for (i = 0; i < deltacloud->services.values_num; i++)
	{
		service = deltacloud->services.values[i];
		if (NULL != service && 0 == strcmp(service->url, url) && 0 == strcmp(service->key, key) && 0 == strcmp(service->secret, secret) && 0 == strcmp(service->driver, driver) && 0 == strcmp(service->provider, provider))
		{
			return service;
		}
	}

	service = __cloud_mem_malloc_func(NULL, sizeof(zbx_deltacloud_service_t));

	memset(service, 0, sizeof(zbx_deltacloud_service_t));

	service->url = cloud_shared_strdup(url);
	service->key = cloud_shared_strdup(key);
	service->secret = cloud_shared_strdup(secret);
	service->driver = cloud_shared_strdup(driver);
	service->provider = cloud_shared_strdup(provider);
	service->lastaccess = time(NULL);
	service->lastcheck = time(NULL);
	CLOUD_VECTOR_CREATE(&service->instances, ptr);

	zbx_vector_ptr_append(&deltacloud->services, service);
	return service;
}

static void metric_monitor(char *url, char *key, char *secret, char *driver, char *provider, zbx_deltacloud_instance_t *instance)
{
	zbx_deltacloud_metric_t	*deltacloud_metric = NULL;
	zbx_deltacloud_metric_value_t	*metric_value = NULL;

	struct deltacloud_api api;
        struct deltacloud_metric *metric = NULL;
        struct deltacloud_metric *start_ptr = NULL;

	int result;

	result = deltacloud_initialize(&api, url, key, secret, driver, provider);

	deltacloud_get_metrics_by_instance_id(&api, instance->id,  &metric);

	if (result == -1 || metric == NULL)
		return;
	
	start_ptr = metric;
	
	while(1){
		deltacloud_metric = __cloud_mem_malloc_func(NULL, sizeof(zbx_deltacloud_metric_t));
		zabbix_log(LOG_LEVEL_ERR, "-------used_size: %d---\n", cloud_mem->used_size);
                deltacloud_metric->name = cloud_shared_strdup(metric->name);
                deltacloud_metric->href = cloud_shared_strdup(metric->href);

		if(metric->values)
		{
			CLOUD_VECTOR_CREATE(&deltacloud_metric->values, ptr);
			metric_value = __cloud_mem_malloc_func(NULL, sizeof(zbx_deltacloud_metric_value_t));	
			metric_value->unit = cloud_shared_strdup(metric->values->unit);
			metric_value->minimum = cloud_shared_strdup(metric->values->minimum);
			metric_value->maximum = cloud_shared_strdup(metric->values->maximum);
			metric_value->samples = cloud_shared_strdup(metric->values->samples);
			metric_value->average = cloud_shared_strdup(metric->values->average);
			zbx_vector_ptr_append(&deltacloud_metric->values, metric_value);
		}
		
		zbx_vector_ptr_append(&instance->metrics, deltacloud_metric);
                metric = metric->next;
                if(metric == NULL)
                	break;
        }
	if(NULL != start_ptr)
  		deltacloud_free_metric_list(&start_ptr);
}
	
int	zbx_module_cloud_instance_discovery(AGENT_REQUEST *request, AGENT_RESULT *result)
{
	zbx_module_item_timeout(0);
	int i;
	struct zbx_json json;
	char	*url;
	char	*key;
	char	*secret;
	char	*driver;
	char	*provider;
	zbx_deltacloud_service_t	*service = NULL;
	zbx_deltacloud_instance_t	*deltacloud_instance = NULL;
	zbx_deltacloud_address_t	*public_address = NULL;
	zbx_deltacloud_address_t	*private_address = NULL;
	zbx_deltacloud_metric_t		*metric = NULL;
	struct deltacloud_api api;
	struct deltacloud_instance *instance = NULL;
	struct deltacloud_instance *start_ptr = NULL;

	if (request->nparam != 5)
	{
		/* set optional error message */
		SET_MSG_RESULT(result, strdup("Invalid number of parameters e.g.) cloud.instance.discovery[url, key, secret, driver, provider]"));
		return SYSINFO_RET_FAIL;
	}
	url = get_rparam(request, 0);
	key = get_rparam(request, 1);
	secret = get_rparam(request, 2);
	driver = get_rparam(request, 3);
	provider = get_rparam(request, 4);

	// json format init
	zbx_json_init(&json, ZBX_JSON_STAT_BUF_LEN);
	// Add "data":[] for LLD format
	zbx_json_addarray(&json, ZBX_PROTO_TAG_DATA);
	service = zbx_deltacloud_get_service(url, key, secret, driver, provider);
	zbx_vector_ptr_clean(&service->instances, (zbx_mem_free_func_t)cloud_instance_shared_free);
	
	deltacloud_initialize(&api, url, key, secret, driver, provider);

	deltacloud_get_instances(&api, &instance);

	if(instance==NULL){
		zbx_json_close(&json);
		//SET_UI64_RESULT(result, 0);
		SET_STR_RESULT(result, strdup(json.buffer));
		zbx_json_free(&json);
		return SYSINFO_RET_OK;
	}
	start_ptr = instance;
	
	while(1){
		deltacloud_instance = __cloud_mem_malloc_func(NULL, sizeof(zbx_deltacloud_instance_t));
		zabbix_log(LOG_LEVEL_ERR, "-------used_size: %d---\n", cloud_mem->used_size);
		deltacloud_instance->href = cloud_shared_strdup(instance->href);
		deltacloud_instance->id = cloud_shared_strdup(instance->id);
		deltacloud_instance->name = cloud_shared_strdup(instance->name);
		deltacloud_instance->owner_id = cloud_shared_strdup(instance->owner_id);
		deltacloud_instance->image_id = cloud_shared_strdup(instance->image_id);
		deltacloud_instance->image_href = cloud_shared_strdup(instance->image_href);
		deltacloud_instance->realm_id = cloud_shared_strdup(instance->realm_id);
		deltacloud_instance->realm_href = cloud_shared_strdup(instance->realm_href);
		deltacloud_instance->state = cloud_shared_strdup(instance->state);
		deltacloud_instance->launch_time = cloud_shared_strdup(instance->launch_time);

		/* Add IP address information */
		CLOUD_VECTOR_CREATE(&deltacloud_instance->public_addresses, ptr);
		CLOUD_VECTOR_CREATE(&deltacloud_instance->private_addresses, ptr);
		public_address = __cloud_mem_malloc_func(NULL, sizeof(zbx_deltacloud_address_t));
		private_address = __cloud_mem_malloc_func(NULL, sizeof(zbx_deltacloud_address_t));

		if(instance->public_addresses)
			public_address->address = cloud_shared_strdup(instance->public_addresses->address);
		if(instance->private_addresses)
			private_address->address = cloud_shared_strdup(instance->private_addresses->address);

		
		zbx_vector_ptr_append(&deltacloud_instance->public_addresses, public_address);
		zbx_vector_ptr_append(&deltacloud_instance->private_addresses, private_address);

		deltacloud_instance->hwp = cloud_hardware_profile_shared_dup(&instance->hwp);
		CLOUD_VECTOR_CREATE(&deltacloud_instance->metrics, ptr);
		metric_monitor(url, key, secret, driver, provider, deltacloud_instance);
		zbx_vector_ptr_append(&service->instances, deltacloud_instance);

		/* Set json data for LLD response */
		
		zbx_json_addobject(&json, NULL);
		if (NULL != instance->name)
			zbx_json_addstring(&json, NAME_MACRO, deltacloud_instance->name, ZBX_JSON_TYPE_STRING);
		if (NULL != instance->id)
			zbx_json_addstring(&json, ID_MACRO, deltacloud_instance->id, ZBX_JSON_TYPE_STRING);
		for (i = 0; deltacloud_instance->public_addresses.values_num; i++)
		{
			zbx_deltacloud_address_t *address = deltacloud_instance->public_addresses.values[i];
			if(NULL != address)
			{
				zbx_json_addstring(&json, PUBLIC_ADDR_MACRO, address->address, ZBX_JSON_TYPE_STRING);
			}
			break; /* ToDo: multi address support */
		}
		
		for (i = 0; deltacloud_instance->private_addresses.values_num; i++)
		{
			zbx_deltacloud_address_t *address = deltacloud_instance->private_addresses.values[i];
			zbx_json_addstring(&json, PRIVATE_ADDR_MACRO, address->address, ZBX_JSON_TYPE_STRING);
			break; /* ToDo: multi address support */
		}
		zbx_json_close(&json);

		instance = instance->next;
		if(instance == NULL){
			break;
		}
	}

	SET_STR_RESULT(result, strdup(json.buffer));
	zbx_json_free(&json);

  	deltacloud_free_instance_list(&start_ptr);
	zabbix_log(LOG_LEVEL_ERR, "---free deltacloud metric list \n");
	
	return SYSINFO_RET_OK;
}

int	zbx_module_cloud_instance_info(AGENT_REQUEST *request, AGENT_RESULT *result)
{

	int	i;
	char	*url;
	char	*key;
	char	*secret;
	char	*driver;
	char	*provider;
	char	*instance_id;
	char	*element;
	
	zbx_deltacloud_service_t	*service = NULL;

	if (request->nparam != 7)
	{
		/* set optional error message */
		SET_MSG_RESULT(result, strdup("Invalid number of parameters e.g.) cloud.instane.info[url, key, secret, driver, provider, instance_id, element]"));
		return SYSINFO_RET_FAIL;
	}
	url = get_rparam(request, 0);
	key = get_rparam(request, 1);
	secret = get_rparam(request, 2);
	driver = get_rparam(request, 3);
	provider = get_rparam(request, 4);
	instance_id = get_rparam(request, 5);
	element = get_rparam(request, 6);

	service = zbx_deltacloud_get_service(url, key, secret, driver, provider);

	if (service == NULL)
	{
		SET_MSG_RESULT(result, strdup("No Data"));
		return SYSINFO_RET_FAIL;
	}
	
	for (i = 0; service->instances.values_num; i++)
	{
		zbx_deltacloud_instance_t *instance = service->instances.values[i];
		if (0 == strcmp(instance->id, instance_id))
		{
			if (0 == strcmp(element, "state"))
				SET_STR_RESULT(result, strdup(instance->state));
			else if (0 == strcmp(element, "owner_id"))
				SET_STR_RESULT(result, strdup(instance->owner_id));
			else if (0 == strcmp(element, "image_id"))
				SET_STR_RESULT(result, strdup(instance->image_id));
			else if (0 == strcmp(element, "image_href"))
				SET_STR_RESULT(result, strdup(instance->image_href));
			else if (0 == strcmp(element, "realm_id"))
				SET_STR_RESULT(result, strdup(instance->realm_id));
			else if (0 == strcmp(element, "realm_href"))
				SET_STR_RESULT(result, strdup(instance->realm_href));
			else if (0 == strcmp(element, "launch_time"))
				SET_STR_RESULT(result, strdup(instance->launch_time));
			else if (0 == strcmp(element, "hwp_href"))
				SET_STR_RESULT(result, strdup(instance->hwp->href));
			else if (0 == strcmp(element, "hwp_id"))
				SET_STR_RESULT(result, strdup(instance->hwp->id));
			else if (0 == strcmp(element, "hwp_name"))
				SET_STR_RESULT(result, strdup(instance->hwp->name));
			else{
				SET_MSG_RESULT(result, strdup("Unsupported element"));
				return SYSINFO_RET_FAIL;
			}
			return SYSINFO_RET_OK;
		}
	}
	SET_MSG_RESULT(result, strdup("Not match data"));
	return SYSINFO_RET_FAIL;
}

int	zbx_module_cloud_metric_discovery(AGENT_REQUEST *request, AGENT_RESULT *result)
{

	struct zbx_json json;
	int	i;
	char	*url;
	char	*key;
	char	*secret;
	char	*driver;
	char	*provider;
	char	*instance_id;
	
	zbx_deltacloud_service_t	*service = NULL;

	if (request->nparam != 6)
	{
		/* set optional error message */
		SET_MSG_RESULT(result, strdup("Invalid number of parameters e.g.) cloud.metric.discovery[url, key, secret, driver, provider, instance_id]"));
		return SYSINFO_RET_FAIL;
	}
	url = get_rparam(request, 0);
	key = get_rparam(request, 1);
	secret = get_rparam(request, 2);
	driver = get_rparam(request, 3);
	provider = get_rparam(request, 4);
	instance_id = get_rparam(request, 5);

	service = zbx_deltacloud_get_service(url, key, secret, driver, provider);
	if (service == NULL)
	{ SET_MSG_RESULT(result, strdup("No Data"));
		return SYSINFO_RET_FAIL;
	}
        
        zbx_deltacloud_instance_t *instance = NULL;
	
	for (i = 0; service->instances.values_num; i++)
	{
		instance = service->instances.values[i];
		if (NULL == instance)
			break;
		if (0 == strcmp(instance->id, instance_id))
			break;
		instance = NULL;
	}
        if (instance == NULL)
        {
		SET_MSG_RESULT(result, strdup("No Data"));
		return SYSINFO_RET_FAIL;
        } 

	zbx_vector_ptr_clean(&instance->metrics, (zbx_mem_free_func_t)cloud_metric_shared_free);
	metric_monitor(url, key, secret, driver, provider, instance);
	// json format init
	zbx_json_init(&json, ZBX_JSON_STAT_BUF_LEN);
	// Add "data":[] for LLD format
	zbx_json_addarray(&json, ZBX_PROTO_TAG_DATA);

	for (i = 0; instance->metrics.values_num; i++)
	{
		zbx_deltacloud_metric_t *metric = instance->metrics.values[i];
		if (NULL == metric)
		{
			zbx_json_close(&json);
			break;
		}
		zbx_json_addobject(&json, NULL);
		if (NULL != metric->name)
		{
			zbx_json_addstring(&json, METRIC_NAME_MACRO, metric->name, ZBX_JSON_TYPE_STRING);
			zbx_json_close(&json);
		}
	}
	SET_STR_RESULT(result, strdup(json.buffer));
	zbx_json_free(&json);
	
	return SYSINFO_RET_OK;
}

int	zbx_module_cloud_metric(AGENT_REQUEST *request, AGENT_RESULT *result)
{
	int	i,j;
	char	*url;
	char	*key;
	char	*secret;
	char	*driver;
	char	*provider;
	char	*instance_id;
	char	*metric_name;
	char	*mode;
	
	zbx_deltacloud_service_t	*service = NULL;

	if (request->nparam != 8)
	{
		/* set optional error message */
		SET_MSG_RESULT(result, strdup("Invalid number of parameters e.g.) cloud.instane.realm_id[url, key, secret, driver, provider, instance_id]"));
		return SYSINFO_RET_FAIL;
	}
	url = get_rparam(request, 0);
	key = get_rparam(request, 1);
	secret = get_rparam(request, 2);
	driver = get_rparam(request, 3);
	provider = get_rparam(request, 4);
	instance_id = get_rparam(request, 5);
	metric_name = get_rparam(request, 6);
	mode = get_rparam(request, 7);

	service = zbx_deltacloud_get_service(url, key, secret, driver, provider);

	if (service == NULL)
	{
		SET_MSG_RESULT(result, strdup("No Data"));
		return SYSINFO_RET_FAIL;
	}
	
	for (i = 0; service->instances.values_num; i++)
	{
		zbx_deltacloud_instance_t *instance = service->instances.values[i];
		if (instance == NULL)
		{
			SET_MSG_RESULT(result, strdup("No instance"));
			return SYSINFO_RET_FAIL;
		}
		if (0 == strcmp(instance->id, instance_id))
		{
			for (j = 0; instance->metrics.values_num; j++)
			{
				zbx_deltacloud_metric_t *metric = instance->metrics.values[j];
				if (metric == NULL)
				{
					SET_MSG_RESULT(result, strdup("No metric"));
					return SYSINFO_RET_FAIL;
				}
				if (NULL != metric->name && 0 == strcmp(metric->name, metric_name))
				{
					if (0 == metric->values.values_num)
					{
						SET_MSG_RESULT(result, strdup("No metric value data"));
						return SYSINFO_RET_FAIL;
					}

					zbx_deltacloud_metric_value_t *metric_value = metric->values.values[0];
					if (NULL == metric_value)
					{	
						SET_MSG_RESULT(result, strdup("No metric value data"));
						return SYSINFO_RET_FAIL;
					}
					if (0 == strcmp(mode, "minimum"))
					{
						SET_STR_RESULT(result, strdup(metric_value->minimum));
					}else if (0 == strcmp(mode, "maximum"))
					{
						SET_STR_RESULT(result, strdup(metric_value->maximum));
					}else if (0 == strcmp(mode, "samples"))
					{
						SET_STR_RESULT(result, strdup(metric_value->samples));
					}else if (0 == strcmp(mode, "average"))
					{
						SET_STR_RESULT(result, strdup(metric_value->average));
					}else
					{
						SET_MSG_RESULT(result, strdup("Not match date mode"));
						return SYSINFO_RET_FAIL;
					}
					return SYSINFO_RET_OK;
				}			
				metric = NULL;
			}
			return SYSINFO_RET_OK;
		}
	}
	SET_MSG_RESULT(result, strdup("Not match data"));
	return SYSINFO_RET_FAIL;
}
/******************************************************************************
 *                                                                            *
 * Function: zbx_module_init                                                  *
 *                                                                            *
 * Purpose: the function is called on agent startup                           *
 *          It should be used to call any initialization routines             *
 *                                                                            *
 * Return value: ZBX_MODULE_OK - success                                      *
 *               ZBX_MODULE_FAIL - module initialization failed               *
 *                                                                            *
 * Comment: the module won't be loaded in case of ZBX_MODULE_FAIL             *
 *                                                                            *
 ******************************************************************************/
int	zbx_module_init()
{
	/* initialization for dummy.random */
	srand(time(NULL));

	key_t shm_key;
	shm_key = zbx_ftok(CONFIG_FILE, ZBX_IPC_CLOUD_ID);
	zbx_mem_create(&cloud_mem, shm_key, ZBX_NO_MUTEX, MEM_SIZE, "cloud cache size", "CloudCacheSize", 0);

	zabbix_log(LOG_LEVEL_ERR, "-------shm_key : %d---\n", shm_key);
	zabbix_log(LOG_LEVEL_ERR, "-------total_size: %d---\n", cloud_mem->total_size);
	zabbix_log(LOG_LEVEL_ERR, "-------used_size: %d---\n", cloud_mem->used_size);
	deltacloud = __cloud_mem_malloc_func(NULL, sizeof(zbx_deltacloud_t));	
	zabbix_log(LOG_LEVEL_ERR, "-------used_size: %d---\n", cloud_mem->used_size);
	memset(deltacloud, 0, sizeof(zbx_deltacloud_t));
	zabbix_log(LOG_LEVEL_ERR, "-------used_size: %d---\n", cloud_mem->used_size);

	CLOUD_VECTOR_CREATE(&deltacloud->services, ptr);

	return ZBX_MODULE_OK;
}

static void	cloud_address_shared_free(zbx_deltacloud_address_t *address)
{
	if (NULL != address->address)
		__cloud_mem_free_func(address->address);
	__cloud_mem_free_func(address);
}

static void	cloud_hardware_profile_shared_free(zbx_deltacloud_hardware_profile_t *hwp)
{
	if (NULL != hwp->href)
		__cloud_mem_free_func(hwp->href);
	if (NULL != hwp->id)
		__cloud_mem_free_func(hwp->id);
	if (NULL != hwp->name)
		__cloud_mem_free_func(hwp->name);
	__cloud_mem_free_func(hwp);
}

static void	cloud_metric_value_shared_free(zbx_deltacloud_metric_value_t *value)
{
	if (NULL != value)
	{
		if (NULL != value->unit)
			__cloud_mem_free_func(value->unit);
		if (NULL != value->minimum)
			__cloud_mem_free_func(value->minimum);
		if (NULL != value->maximum)
			__cloud_mem_free_func(value->maximum);
		if (NULL != value->samples)
			__cloud_mem_free_func(value->samples);
		if (NULL != value->average)
			__cloud_mem_free_func(value->average);
	}
	__cloud_mem_free_func(value);
}

static void	cloud_metric_shared_free(zbx_deltacloud_metric_t *metric)
{

	zbx_vector_ptr_clean(&metric->values, (zbx_mem_free_func_t)cloud_metric_value_shared_free);
	zbx_vector_ptr_destroy(&metric->values);
	if (NULL != metric->href)
		__cloud_mem_free_func(metric->href);
	if (NULL != metric->name)
		__cloud_mem_free_func(metric->name);
	__cloud_mem_free_func(metric);
}

static void	cloud_instance_shared_free(zbx_deltacloud_instance_t *instance)
{
	if (NULL != instance->href)
		__cloud_mem_free_func(instance->href);
	if (NULL != instance->id)
		__cloud_mem_free_func(instance->id);
	if (NULL != instance->name)
		__cloud_mem_free_func(instance->name);
	if (NULL != instance->owner_id)
		__cloud_mem_free_func(instance->owner_id);
	if (NULL != instance->image_id)
		__cloud_mem_free_func(instance->image_id);
	if (NULL != instance->image_href)
		__cloud_mem_free_func(instance->image_href);
	if (NULL != instance->realm_id)
		__cloud_mem_free_func(instance->realm_id);
	if (NULL != instance->realm_href)
		__cloud_mem_free_func(instance->realm_href);
	if (NULL != instance->state)
		__cloud_mem_free_func(instance->state);
	if (NULL != instance->launch_time)
		__cloud_mem_free_func(instance->launch_time);
	zbx_vector_ptr_clean(&instance->public_addresses, (zbx_mem_free_func_t)cloud_address_shared_free);
	zbx_vector_ptr_clean(&instance->private_addresses, (zbx_mem_free_func_t)cloud_address_shared_free);
	
	zbx_vector_ptr_clean(&instance->metrics, (zbx_mem_free_func_t)cloud_metric_shared_free);
	zbx_vector_ptr_destroy(&instance->public_addresses);
	zbx_vector_ptr_destroy(&instance->private_addresses);
	zbx_vector_ptr_destroy(&instance->metrics);
	cloud_hardware_profile_shared_free(instance->hwp);
	__cloud_mem_free_func(instance);
}

static void	cloud_service_shared_free(zbx_deltacloud_service_t *service)
{
	if (NULL != service->url)
		__cloud_mem_free_func(service->url);
	if (NULL != service->key)
		__cloud_mem_free_func(service->key);
	if (NULL != service->secret)
		__cloud_mem_free_func(service->secret);
	if (NULL != service->driver)
		__cloud_mem_free_func(service->driver);
	if (NULL != service->provider)
		__cloud_mem_free_func(service->provider);

	zbx_vector_ptr_clean(&service->instances, (zbx_mem_free_func_t)cloud_instance_shared_free);
	zbx_vector_ptr_destroy(&service->instances);
	__cloud_mem_free_func(service);
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_module_uninit                                                *
 *                                                                            *
 * Purpose: the function is called on agent shutdown                          *
 *          It should be used to cleanup used resources if there are any      *
 *                                                                            *
 * Return value: ZBX_MODULE_OK - success                                      *
 *               ZBX_MODULE_FAIL - function failed                            *
 *                                                                            *
 ******************************************************************************/
int	zbx_module_uninit()
{
	if (NULL != deltacloud)
	{
		zbx_vector_ptr_clean(&deltacloud->services, (zbx_mem_free_func_t)cloud_service_shared_free);
		zbx_vector_ptr_destroy(&deltacloud->services);
		__cloud_mem_free_func(deltacloud);
	}
	zbx_mem_destroy(cloud_mem);
	zabbix_log(LOG_LEVEL_ERR, "----destroy cloud_mem---used_size: %d---\n", cloud_mem->used_size);

	return ZBX_MODULE_OK;
}
