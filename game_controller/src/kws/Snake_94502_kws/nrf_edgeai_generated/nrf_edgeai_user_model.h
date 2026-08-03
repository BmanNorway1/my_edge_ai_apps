/* 2026-07-13T09:29:13.095667 */

/*
* Copyright (c) 2026 Nordic Semiconductor ASA
* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
*/

#ifndef _NRF_EDGEAI_USER_MODEL_94502_H_
#define _NRF_EDGEAI_USER_MODEL_94502_H_

#include <nrf_edgeai/rt/nrf_edgeai_types.h>

#ifdef __cplusplus 
extern "C" {
#endif

/** 
 * @brief Get pointer to the Nordic Edge AI Lab model instance (@ref nrf_edgeai_t).
 */
nrf_edgeai_t* nrf_edgeai_user_model_94502(void);
/** 
 * @brief Get size FLASH/ROM size of the Nordic Edge AI model. 
 * 
 * @return Size in bytes of the model.
 */ 
uint32_t nrf_edgeai_user_model_size_94502(void);

/** 
 * @brief Alias for the Nordic Edge AI Lab user model API name.
 */
#ifndef nrf_edgeai_user_model
#define nrf_edgeai_user_model nrf_edgeai_user_model_94502
#endif

/** 
 * @brief Alias for the Nordic Edge AI Lab user model size API name.
 */
#ifndef nrf_edgeai_user_model_size
#define nrf_edgeai_user_model_size nrf_edgeai_user_model_size_94502
#endif

#ifdef __cplusplus 
}
#endif 

#endif /* _NRF_EDGEAI_USER_MODEL_94502_H_ */
