#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <time.h>
#include <unistd.h>

#include "rk_debug.h"
#include "rk_defines.h"
#include "rk_mpi_adec.h"
#include "rk_mpi_aenc.h"
#include "rk_mpi_ai.h"
#include "rk_mpi_ao.h"
#include "rk_mpi_avs.h"
#include "rk_mpi_cal.h"
#include "rk_mpi_ivs.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_rgn.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_tde.h"
#include "rk_mpi_vdec.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_vo.h"
#include "rk_mpi_vpss.h"

// Заголовки для ISP
#include <rk_aiq_user_api2_camgroup.h>
#include <rk_aiq_user_api2_sysctl.h>

// Глобальные переменные
static rk_aiq_sys_ctx_t *g_aiq_ctx = NULL;
static bool quit = false;
FILE *file = NULL;
static const RK_CHAR *g_pOutPath = "/tmp/";
static RK_U32 g_u32FrameCount = 0;

/*
 * Пропускаем первые SKIP_FRAME_NUM кадров (даём ISP время
 * доотстроить экспозицию/баланс белого на уже стабилизированном
 * потоке), сохраняем только последний — 15-й.
 */
static const RK_U32 SKIP_FRAME_NUM = 14;
static const RK_U32 TOTAL_FRAME_NUM = SKIP_FRAME_NUM + 1; // 15
static RK_U32 g_frame_counter = 0;                        // все полученные кадры (пропуск + финальный)
static RK_U32 g_saved_frames = 0;                          // 0 или 1 — сохранён ли финальный кадр

static void sigterm_handler(int sig) {
	fprintf(stderr, "signal %d\n", sig);
	quit = true;
}

// Callback для ISP
static XCamReturn SIMPLE_COMM_ISP_SofCb(rk_aiq_metas_t *meta) {
	(void)meta;
	g_u32FrameCount++;
	return XCAM_RETURN_NO_ERROR;
}

static XCamReturn SIMPLE_COMM_ISP_ErrCb(rk_aiq_err_msg_t *msg) {
	(void)msg;
	return XCAM_RETURN_NO_ERROR;
}

/*
 * Корректная остановка/деинициализация ISP.
 * Вынесена в отдельную функцию, чтобы вызывать её из ЛЮБОЙ точки выхода
 * из main, и чтобы после деинита g_aiq_ctx гарантированно был NULL —
 * это защищает от повторного деинита и от "зависших" хендлов, из-за
 * которых следующий запуск не может открыть камеру.
 */
static void SIMPLE_COMM_ISP_Stop(void) {
	if (!g_aiq_ctx) {
		return;
	}
	printf("Stopping ISP...\n");
	RK_S32 ret = rk_aiq_uapi2_sysctl_stop(g_aiq_ctx, false);
	if (ret != 0) {
		printf("rk_aiq_uapi2_sysctl_stop failed: %d\n", ret);
	}
	rk_aiq_uapi2_sysctl_deinit(g_aiq_ctx);
	g_aiq_ctx = NULL;
	printf("ISP stopped\n");
}

// Функция для получения JPEG и сохранения в файл
static void *GetMediaBuffer0(void *arg) {
	(void)arg;
	printf("========%s========\n", __func__);
	void *pData = RK_NULL;
	int s32Ret;
	char jpeg_path[128];

	VENC_STREAM_S stFrame;
	stFrame.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));
	if (!stFrame.pstPack) {
		printf("Failed to allocate memory for VENC_PACK_S\n");
		return NULL;
	}

	printf("Skipping first %d frames (ISP calibration), saving only frame #%d...\n",
	       SKIP_FRAME_NUM, TOTAL_FRAME_NUM);

	while (!quit && g_frame_counter < TOTAL_FRAME_NUM) {
		s32Ret = RK_MPI_VENC_GetStream(0, &stFrame, 500);
		if (s32Ret == RK_SUCCESS) {
			g_frame_counter++;

			if (g_frame_counter < TOTAL_FRAME_NUM) {
				// "Разогревочный" кадр — просто пропускаем, не сохраняем
				printf("Skip warm-up frame %d/%d\n", g_frame_counter, SKIP_FRAME_NUM);
			} else {
				// Это 15-й (последний) кадр — сохраняем его
				memset(jpeg_path, 0, sizeof(jpeg_path));
				snprintf(jpeg_path, sizeof(jpeg_path), "%s/image.jpg", g_pOutPath);
				file = fopen(jpeg_path, "wb");

				if (file) {
					pData = RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
					fwrite(pData, 1, stFrame.pstPack->u32Len, file);
					fflush(file);
					fclose(file);
					file = NULL;
					g_saved_frames = 1;
					printf("Saved frame %d/%d: %s (size: %d bytes)\n", g_frame_counter,
					       TOTAL_FRAME_NUM, jpeg_path, stFrame.pstPack->u32Len);
				} else {
					printf("Failed to open file: %s\n", jpeg_path);
				}
			}

			s32Ret = RK_MPI_VENC_ReleaseStream(0, &stFrame);
			if (s32Ret != RK_SUCCESS) {
				RK_LOGE("RK_MPI_VENC_ReleaseStream fail %x", s32Ret);
			}

			if (g_frame_counter >= TOTAL_FRAME_NUM) {
				printf("\nDone: skipped %d frames, saved frame #%d!\n", SKIP_FRAME_NUM,
				       TOTAL_FRAME_NUM);
				quit = true;
				break;
			}
		}
	}
	free(stFrame.pstPack);
	return NULL;
}

static RK_S32 test_venc_init(int chnId, int width, int height, RK_CODEC_ID_E enType) {
	printf("========%s========\n", __func__);
	VENC_RECV_PIC_PARAM_S stRecvParam;
	VENC_CHN_ATTR_S stAttr;
	VENC_CHN_PARAM_S stParam;
	memset(&stAttr, 0, sizeof(VENC_CHN_ATTR_S));
	memset(&stParam, 0, sizeof(VENC_CHN_PARAM_S));

	stAttr.stVencAttr.enType = enType;
	stAttr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
	stAttr.stVencAttr.u32PicWidth = width;
	stAttr.stVencAttr.u32PicHeight = height;
	stAttr.stVencAttr.u32VirWidth = width;
	stAttr.stVencAttr.u32VirHeight = height;
	stAttr.stVencAttr.u32StreamBufCnt = 2;
	stAttr.stVencAttr.u32BufSize = width * height * 3 / 2;
	stAttr.stVencAttr.enMirror = MIRROR_NONE;

	stAttr.stVencAttr.stAttrJpege.bSupportDCF = RK_FALSE;
	stAttr.stVencAttr.stAttrJpege.stMPFCfg.u8LargeThumbNailNum = 0;
	stAttr.stVencAttr.stAttrJpege.enReceiveMode = VENC_PIC_RECEIVE_SINGLE;

	RK_MPI_VENC_CreateChn(chnId, &stAttr);

	stParam.stFrameRate.bEnable = RK_FALSE;
	stParam.stFrameRate.s32SrcFrmRateNum = 25;
	stParam.stFrameRate.s32SrcFrmRateDen = 1;
	stParam.stFrameRate.s32DstFrmRateNum = 10;
	stParam.stFrameRate.s32DstFrmRateDen = 1;
	RK_MPI_VENC_SetChnParam(chnId, &stParam);

	memset(&stRecvParam, 0, sizeof(VENC_RECV_PIC_PARAM_S));
	stRecvParam.s32RecvPicNum = 1;
	RK_MPI_VENC_StartRecvFrame(chnId, &stRecvParam);

	return 0;
}

int vi_dev_init(void) {
	printf("%s\n", __func__);
	int ret = 0;
	int devId = 0;
	int pipeId = devId;

	VI_DEV_ATTR_S stDevAttr;
	VI_DEV_BIND_PIPE_S stBindPipe;
	memset(&stDevAttr, 0, sizeof(stDevAttr));
	memset(&stBindPipe, 0, sizeof(stBindPipe));

	ret = RK_MPI_VI_GetDevAttr(devId, &stDevAttr);
	if (ret == RK_ERR_VI_NOT_CONFIG) {
		ret = RK_MPI_VI_SetDevAttr(devId, &stDevAttr);
		if (ret != RK_SUCCESS) {
			printf("RK_MPI_VI_SetDevAttr %x\n", ret);
			return -1;
		}
	} else {
		printf("RK_MPI_VI_SetDevAttr already\n");
	}

	ret = RK_MPI_VI_GetDevIsEnable(devId);
	if (ret != RK_SUCCESS) {
		ret = RK_MPI_VI_EnableDev(devId);
		if (ret != RK_SUCCESS) {
			printf("RK_MPI_VI_EnableDev %x\n", ret);
			return -1;
		}
		stBindPipe.u32Num = 1;
		stBindPipe.PipeId[0] = pipeId;
		ret = RK_MPI_VI_SetDevBindPipe(devId, &stBindPipe);
		if (ret != RK_SUCCESS) {
			printf("RK_MPI_VI_SetDevBindPipe %x\n", ret);
			return -1;
		}
	} else {
		printf("RK_MPI_VI_EnableDev already\n");
	}

	return 0;
}

int vi_chn_init(int channelId, int width, int height) {
	int ret;
	int buf_cnt = 2;
	VI_CHN_ATTR_S vi_chn_attr;
	memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
	vi_chn_attr.stIspOpt.u32BufCount = buf_cnt;
	vi_chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
	vi_chn_attr.stSize.u32Width = width;
	vi_chn_attr.stSize.u32Height = height;
	vi_chn_attr.enPixelFormat = RK_FMT_YUV420SP;
	vi_chn_attr.enCompressMode = COMPRESS_MODE_NONE;
	vi_chn_attr.u32Depth = 0;
	ret = RK_MPI_VI_SetChnAttr(0, channelId, &vi_chn_attr);
	ret |= RK_MPI_VI_EnableChn(0, channelId);
	if (ret) {
		printf("ERROR: create VI error! ret=%d\n", ret);
		return ret;
	}

	return ret;
}

static const RK_CHAR optstr[] = "?::w:h:I:e:";
static void print_usage(const RK_CHAR *name) {
	printf("usage example:\n");
	printf("\t%s -I 0 -w 1920 -h 1080 \n", name);
	printf("\t-w | --width: VI width, Default:1920\n");
	printf("\t-h | --heght: VI height, Default:1080\n");
	printf("\t-I | --camid: camera ctx id, Default 0. "
	       "0:rkisp_mainpath,1:rkisp_selfpath,2:rkisp_bypasspath\n");
}

int main(int argc, char *argv[]) {
	RK_S32 s32Ret = RK_FAILURE;
	RK_U32 u32Width = 1920;
	RK_U32 u32Height = 1080;
	RK_CODEC_ID_E enCodecType = RK_VIDEO_ID_JPEG;
	RK_S32 s32chnlId = 0;
	int c;
	int ret = -1;
	bool sys_inited = false;
	bool vi_bound = false;

	while ((c = getopt(argc, argv, (char *)optstr)) != -1) {
		switch (c) {
		case 'w':
			u32Width = atoi(optarg);
			break;
		case 'h':
			u32Height = atoi(optarg);
			break;
		case 'I':
			s32chnlId = atoi(optarg);
			break;
		case '?':
		default:
			print_usage(argv[0]);
			return -1;
		}
	}

	printf("#Resolution: %dx%d\n", u32Width, u32Height);
	printf("#CameraIdx: %d\n\n", s32chnlId);
	printf("Will skip %d frames, then save frame #%d, to %s\n", SKIP_FRAME_NUM,
	       TOTAL_FRAME_NUM, g_pOutPath);

	signal(SIGINT, sigterm_handler);

	if (RK_MPI_SYS_Init() != RK_SUCCESS) {
		RK_LOGE("rk mpi sys init fail!");
		goto __CLEANUP;
	}
	sys_inited = true;

	// 1. Инициализируем ISP ПЕРЕД VI
	{
		printf("Initializing ISP...\n");
		const char *iq_dir = "/etc/iqfiles";

		rk_aiq_static_info_t aiq_static_info;
		RK_S32 ispRet = rk_aiq_uapi2_sysctl_enumStaticMetas(0, &aiq_static_info);
		if (ispRet == 0) {
			printf("Sensor: %s\n", aiq_static_info.sensor_info.sensor_name);

			g_aiq_ctx = rk_aiq_uapi2_sysctl_init(aiq_static_info.sensor_info.sensor_name,
			                                      iq_dir, SIMPLE_COMM_ISP_ErrCb,
			                                      SIMPLE_COMM_ISP_SofCb);

			if (g_aiq_ctx) {
				printf("ISP init success\n");
				if (rk_aiq_uapi2_sysctl_prepare(g_aiq_ctx, 0, 0,
				                                RK_AIQ_WORKING_MODE_NORMAL) == 0) {
					printf("ISP prepare success\n");
					if (rk_aiq_uapi2_sysctl_start(g_aiq_ctx) == 0) {
						printf("ISP started successfully\n");
					}
				}
			} else {
				printf("ISP init failed!\n");
			}
		} else {
			printf("Failed to enumerate sensor metadata: %d\n", ispRet);
		}

		printf("Waiting for ISP stabilization...\n");
		sleep(2);
	}

	// 2. Инициализируем VI
	vi_dev_init();
	vi_chn_init(s32chnlId, u32Width, u32Height);

	// 3. Инициализируем VENC
	test_venc_init(0, u32Width, u32Height, enCodecType);

	// 4. Привязываем VI к VENC
	{
		MPP_CHN_S stSrcChn, stDestChn;
		stSrcChn.enModId = RK_ID_VI;
		stSrcChn.s32DevId = 0;
		stSrcChn.s32ChnId = s32chnlId;

		stDestChn.enModId = RK_ID_VENC;
		stDestChn.s32DevId = 0;
		stDestChn.s32ChnId = 0;

		printf("====RK_MPI_SYS_Bind vi%d to venc0====\n", s32chnlId);
		s32Ret = RK_MPI_SYS_Bind(&stSrcChn, &stDestChn);
		if (s32Ret != RK_SUCCESS) {
			RK_LOGE("bind vi to venc failed, ret=%d", s32Ret);
			goto __CLEANUP;
		}
		vi_bound = true;

		// 5. Создаём поток для получения JPEG
		pthread_t main_thread;
		pthread_create(&main_thread, NULL, GetMediaBuffer0, NULL);

		// Настройка качества JPEG
		VENC_JPEG_PARAM_S stJpegParam;
		memset(&stJpegParam, 0, sizeof(stJpegParam));
		stJpegParam.u32Qfactor = 85;
		RK_MPI_VENC_SetJpegParam(0, &stJpegParam);

		// 6. Запускаем захват кадров
		VENC_RECV_PIC_PARAM_S stRecvParam;

		printf("\n=== Starting capture ===\n");
		printf("Requesting %d frames (skip %d, save #%d)...\n", TOTAL_FRAME_NUM,
		       SKIP_FRAME_NUM, TOTAL_FRAME_NUM);

		for (RK_U32 i = 0; i < TOTAL_FRAME_NUM && !quit; i++) {
			memset(&stRecvParam, 0, sizeof(VENC_RECV_PIC_PARAM_S));
			stRecvParam.s32RecvPicNum = 1;
			s32Ret = RK_MPI_VENC_StartRecvFrame(0, &stRecvParam);
			if (s32Ret) {
				printf("RK_MPI_VENC_StartRecvFrame failed at frame %d!\n", i);
				break;
			}
			printf("Sending capture request %d/%d...\n", i + 1, TOTAL_FRAME_NUM);
			usleep(50000); // 50ms между кадрами
		}

		// Ждём, пока поток не получит все TOTAL_FRAME_NUM кадров
		// (14 пропущенных + 1 сохранённый)
		while (!quit && g_frame_counter < TOTAL_FRAME_NUM) {
			usleep(100000);
		}

		quit = true;
		pthread_join(main_thread, NULL);

		printf("\n=== Capture complete ===\n");
		if (g_saved_frames > 0) {
			printf("Saved frame #%d to %s/image.jpg\n", TOTAL_FRAME_NUM, g_pOutPath);
		} else {
			printf("WARNING: final frame was not saved (got %d/%d frames)\n",
			       g_frame_counter, TOTAL_FRAME_NUM);
		}
	}

	ret = 0;

__CLEANUP:
	/*
	 * ВАЖНО: ниже НЕТ ни одного "return" до конца функции.
	 * Раньше здесь был преждевременный return после
	 * RK_MPI_VENC_StopRecvFrame, который пропускал VI_DisableDev,
	 * деинит ISP и RK_MPI_SYS_Exit — из-за этого ресурсы (VI-девайс,
	 * ISP-контекст) оставались занятыми, и следующий запуск не мог
	 * открыть камеру. Теперь каждый шаг только логирует ошибку и
	 * выполнение идёт дальше, чтобы деинициализация была полной
	 * при любом исходе.
	 */
	if (vi_bound) {
		MPP_CHN_S stSrcChn, stDestChn;
		stSrcChn.enModId = RK_ID_VI;
		stSrcChn.s32DevId = 0;
		stSrcChn.s32ChnId = s32chnlId;
		stDestChn.enModId = RK_ID_VENC;
		stDestChn.s32DevId = 0;
		stDestChn.s32ChnId = 0;

		s32Ret = RK_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
		if (s32Ret != RK_SUCCESS) {
			RK_LOGE("RK_MPI_SYS_UnBind fail %x", s32Ret);
		}
	}

	s32Ret = RK_MPI_VI_DisableChn(0, s32chnlId);
	if (s32Ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VI_DisableChn fail %x", s32Ret);
	}

	s32Ret = RK_MPI_VENC_StopRecvFrame(0);
	if (s32Ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VENC_StopRecvFrame fail %x", s32Ret);
	}

	s32Ret = RK_MPI_VENC_DestroyChn(0);
	if (s32Ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VENC_DestroyChn fail %x", s32Ret);
	}

	s32Ret = RK_MPI_VI_DisableDev(0);
	if (s32Ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VI_DisableDev fail %x", s32Ret);
	}

	if (sys_inited) {
		RK_MPI_SYS_Exit();
	}

	/*
	 * ISP останавливаем ПОСЛЕ RK_MPI_SYS_Exit() — именно так это
	 * сделано в рабочем RTSP-примере. Если стопить ISP раньше, пока
	 * MPI-подсистема ещё держит VI/медиа-линки, деинит ISP может не
	 * до конца освободить v4l2-хендлы сенсора.
	 */
	SIMPLE_COMM_ISP_Stop();

	return ret;
}