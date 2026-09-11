#pragma once

#include "generated/slang.cuh"

struct DiffPair_float_0
{
    float primal_0;
    float differential_0;
};

inline __device__ void _d_max_0(DiffPair_float_0 * dpx_0, DiffPair_float_0 * dpy_0, float dOut_0)
{
    DiffPair_float_0 _S1 = *dpx_0;
    float _S2;
    if(((*dpx_0).primal_0) > ((*dpy_0).primal_0))
    {
        _S2 = dOut_0;
    }
    else
    {
        if(((*dpx_0).primal_0) < ((*dpy_0).primal_0))
        {
            _S2 = 0.0f;
        }
        else
        {
            _S2 = 0.5f * dOut_0;
        }
    }
    dpx_0->primal_0 = _S1.primal_0;
    dpx_0->differential_0 = _S2;
    DiffPair_float_0 _S3 = *dpy_0;
    if(((*dpy_0).primal_0) > (_S1.primal_0))
    {
        _S2 = dOut_0;
    }
    else
    {
        if(((*dpy_0).primal_0) < ((*dpx_0).primal_0))
        {
            _S2 = 0.0f;
        }
        else
        {
            _S2 = 0.5f * dOut_0;
        }
    }
    dpy_0->primal_0 = _S3.primal_0;
    dpy_0->differential_0 = _S2;
    return;
}

inline __device__ float rendered_depth_to_expected_depth(float depth_0, float transmittance_0)
{
    return depth_0 / (F32_max((1.0f - transmittance_0), (1.00000001335143196e-10f)));
}

inline __device__ void s_bwd_prop_rendered_depth_to_expected_depth_0(DiffPair_float_0 * dpdepth_0, DiffPair_float_0 * dptransmittance_0, float _s_dOut_0)
{
    float _S4 = 1.0f - (*dptransmittance_0).primal_0;
    float _S5 = (F32_max((_S4), (1.00000001335143196e-10f)));
    float _S6 = _s_dOut_0 / (_S5 * _S5);
    float _S7 = (*dpdepth_0).primal_0 * - _S6;
    float _S8 = _S5 * _S6;
    DiffPair_float_0 _S9;
    (&_S9)->primal_0 = _S4;
    (&_S9)->differential_0 = 0.0f;
    DiffPair_float_0 _S10;
    (&_S10)->primal_0 = 1.00000001335143196e-10f;
    (&_S10)->differential_0 = 0.0f;
    _d_max_0(&_S9, &_S10, _S7);
    float _S11 = - _S9.differential_0;
    dptransmittance_0->primal_0 = (*dptransmittance_0).primal_0;
    dptransmittance_0->differential_0 = _S11;
    dpdepth_0->primal_0 = (*dpdepth_0).primal_0;
    dpdepth_0->differential_0 = _S8;
    return;
}

inline __device__ void s_bwd_rendered_depth_to_expected_depth_0(DiffPair_float_0 * _S12, DiffPair_float_0 * _S13, float _S14)
{
    s_bwd_prop_rendered_depth_to_expected_depth_0(_S12, _S13, _S14);
    return;
}

inline __device__ void rendered_depth_to_expected_depth_bwd(float depth_1, float transmittance_1, float v_out_depth_0, float * v_depth_0, float * v_transmittance_0)
{
    DiffPair_float_0 p_depth_0;
    (&p_depth_0)->primal_0 = depth_1;
    (&p_depth_0)->differential_0 = 0.0f;
    DiffPair_float_0 p_transmittance_0;
    (&p_transmittance_0)->primal_0 = transmittance_1;
    (&p_transmittance_0)->differential_0 = 0.0f;
    s_bwd_rendered_depth_to_expected_depth_0(&p_depth_0, &p_transmittance_0, v_out_depth_0);
    *v_depth_0 = p_depth_0.differential_0;
    *v_transmittance_0 = p_transmittance_0.differential_0;
    return;
}

struct DiffPair_vectorx3Cfloatx2C3x3E_0
{
    float3  primal_0;
    float3  differential_0;
};

inline __device__ void _d_dot_0(DiffPair_vectorx3Cfloatx2C3x3E_0 * dpx_1, DiffPair_vectorx3Cfloatx2C3x3E_0 * dpy_1, float dOut_1)
{
    float3  x_d_result_0;
    *&((&x_d_result_0)->x) = (*dpy_1).primal_0.x * dOut_1;
    float3  y_d_result_0;
    *&((&y_d_result_0)->x) = (*dpx_1).primal_0.x * dOut_1;
    *&((&x_d_result_0)->y) = (*dpy_1).primal_0.y * dOut_1;
    *&((&y_d_result_0)->y) = (*dpx_1).primal_0.y * dOut_1;
    *&((&x_d_result_0)->z) = (*dpy_1).primal_0.z * dOut_1;
    *&((&y_d_result_0)->z) = (*dpx_1).primal_0.z * dOut_1;
    dpx_1->primal_0 = (*dpx_1).primal_0;
    dpx_1->differential_0 = x_d_result_0;
    dpy_1->primal_0 = (*dpy_1).primal_0;
    dpy_1->differential_0 = y_d_result_0;
    return;
}

inline __device__ float dot_0(float3  x_0, float3  y_0)
{
    int i_0 = int(0);
    float result_0 = 0.0f;
    for(;;)
    {
        if(i_0 < int(3))
        {
        }
        else
        {
            break;
        }
        float result_1 = result_0 + _slang_vector_get_element(x_0, i_0) * _slang_vector_get_element(y_0, i_0);
        i_0 = i_0 + int(1);
        result_0 = result_1;
    }
    return result_0;
}

inline __device__ float dot_1(float2  x_1, float2  y_1)
{
    int i_1 = int(0);
    float result_2 = 0.0f;
    for(;;)
    {
        if(i_1 < int(2))
        {
        }
        else
        {
            break;
        }
        float result_3 = result_2 + _slang_vector_get_element(x_1, i_1) * _slang_vector_get_element(y_1, i_1);
        i_1 = i_1 + int(1);
        result_2 = result_3;
    }
    return result_2;
}

inline __device__ void blend_background_bwd_impl_0(DiffPair_vectorx3Cfloatx2C3x3E_0 * dp_rgb_0, DiffPair_float_0 * dp_transmittance_0, DiffPair_vectorx3Cfloatx2C3x3E_0 * dp_background_0, float3  v_out_0)
{
    DiffPair_float_0 _S15 = *dp_transmittance_0;
    DiffPair_vectorx3Cfloatx2C3x3E_0 _S16 = *dp_background_0;
    dp_rgb_0->primal_0 = (*dp_rgb_0).primal_0;
    dp_rgb_0->differential_0 = v_out_0;
    float _S17 = dot_0(_S16.primal_0, v_out_0);
    dp_transmittance_0->primal_0 = _S15.primal_0;
    dp_transmittance_0->differential_0 = _S17;
    float3  _S18 = make_float3 (_S15.primal_0) * v_out_0;
    dp_background_0->primal_0 = _S16.primal_0;
    dp_background_0->differential_0 = _S18;
    return;
}

inline __device__ float3  blend_background(float3  rgb_0, float transmittance_2, float3  background_0)
{
    return rgb_0 + make_float3 (transmittance_2) * background_0;
}

inline __device__ float3  min_0(float3  x_2, float3  y_2)
{
    float3  result_4;
    int i_2 = int(0);
    for(;;)
    {
        if(i_2 < int(3))
        {
        }
        else
        {
            break;
        }
        *_slang_vector_get_element_ptr(&result_4, i_2) = (F32_min((_slang_vector_get_element(x_2, i_2)), (_slang_vector_get_element(y_2, i_2))));
        i_2 = i_2 + int(1);
    }
    return result_4;
}

inline __device__ float3  max_0(float3  x_3, float3  y_3)
{
    float3  result_5;
    int i_3 = int(0);
    for(;;)
    {
        if(i_3 < int(3))
        {
        }
        else
        {
            break;
        }
        *_slang_vector_get_element_ptr(&result_5, i_3) = (F32_max((_slang_vector_get_element(x_3, i_3)), (_slang_vector_get_element(y_3, i_3))));
        i_3 = i_3 + int(1);
    }
    return result_5;
}

inline __device__ float3  overexposure_grad(float3  c_0, float scale_0)
{
    float3  _S19 = make_float3 (0.0f);
    return make_float3 (scale_0) * (min_0(c_0, _S19) + max_0(c_0 - make_float3 (1.0f), _S19));
}

inline __device__ void blend_background_bwd(float3  rgb_1, float transmittance_3, float3  background_1, float3  v_out_rgb_0, float overexposure_scale_0, float3  * v_rgb_0, float * v_transmittance_1, float3  * v_background_0)
{
    float3  _S20;
    if(overexposure_scale_0 != 0.0f)
    {
        _S20 = v_out_rgb_0 + overexposure_grad(rgb_1 + make_float3 (transmittance_3) * background_1, overexposure_scale_0);
    }
    else
    {
        _S20 = v_out_rgb_0;
    }
    float3  _S21 = make_float3 (0.0f);
    DiffPair_vectorx3Cfloatx2C3x3E_0 p_rgb_0;
    (&p_rgb_0)->primal_0 = rgb_1;
    (&p_rgb_0)->differential_0 = _S21;
    DiffPair_float_0 p_transmittance_1;
    (&p_transmittance_1)->primal_0 = transmittance_3;
    (&p_transmittance_1)->differential_0 = 0.0f;
    DiffPair_vectorx3Cfloatx2C3x3E_0 p_background_0;
    (&p_background_0)->primal_0 = background_1;
    (&p_background_0)->differential_0 = _S21;
    blend_background_bwd_impl_0(&p_rgb_0, &p_transmittance_1, &p_background_0, _S20);
    *v_rgb_0 = p_rgb_0.differential_0;
    *v_transmittance_1 = p_transmittance_1.differential_0;
    *v_background_0 = p_background_0.differential_0;
    return;
}

inline __device__ void _d_pow_0(DiffPair_float_0 * dpx_2, DiffPair_float_0 * dpy_2, float dOut_2)
{
    if(((*dpx_2).primal_0) < 9.99999997475242708e-07f)
    {
        dpx_2->primal_0 = (*dpx_2).primal_0;
        dpx_2->differential_0 = 0.0f;
        dpy_2->primal_0 = (*dpy_2).primal_0;
        dpy_2->differential_0 = 0.0f;
    }
    else
    {
        float val_0 = (F32_pow(((*dpx_2).primal_0), ((*dpy_2).primal_0)));
        DiffPair_float_0 _S22 = *dpx_2;
        float _S23 = val_0 * (*dpy_2).primal_0 / (*dpx_2).primal_0 * dOut_2;
        dpx_2->primal_0 = (*dpx_2).primal_0;
        dpx_2->differential_0 = _S23;
        float _S24 = val_0 * (F32_log((_S22.primal_0))) * dOut_2;
        dpy_2->primal_0 = (*dpy_2).primal_0;
        dpy_2->differential_0 = _S24;
    }
    return;
}

inline __device__ DiffPair_float_0 _d_pow_1(DiffPair_float_0 * dpx_3, DiffPair_float_0 * dpy_3)
{
    float _S25 = dpx_3->primal_0;
    if((dpx_3->primal_0) < 9.99999997475242708e-07f)
    {
        DiffPair_float_0 _S26 = { 0.0f, 0.0f };
        return _S26;
    }
    float val_1 = (F32_pow((_S25), (dpy_3->primal_0)));
    DiffPair_float_0 _S27 = { val_1, val_1 * (F32_log((_S25))) * dpy_3->differential_0 + val_1 * dpy_3->primal_0 / _S25 * dpx_3->differential_0 };
    return _S27;
}

inline __device__ float linear_rgb_to_srgb(float x_4)
{
    float _S28;
    if(x_4 < 0.00313080009073019f)
    {
        _S28 = x_4 * 12.92000007629394531f;
    }
    else
    {
        _S28 = 1.0549999475479126f * (F32_pow((x_4), (0.4166666567325592f))) - 0.05499999970197678f;
    }
    return _S28;
}

inline __device__ float linear_rgb_to_srgb_grad(float x_5)
{
    float _S29;
    if(x_5 < 0.00313080009073019f)
    {
        _S29 = 12.92000007629394531f;
    }
    else
    {
        DiffPair_float_0 _S30;
        (&_S30)->primal_0 = x_5;
        (&_S30)->differential_0 = 1.0f;
        DiffPair_float_0 _S31;
        (&_S31)->primal_0 = 0.4166666567325592f;
        (&_S31)->differential_0 = 0.0f;
        DiffPair_float_0 _S32 = _d_pow_1(&_S30, &_S31);
        _S29 = _S32.differential_0 * 1.0549999475479126f;
    }
    return _S29;
}

inline __device__ float srgb_to_linear_rgb(float x_6)
{
    float _S33;
    if(x_6 < 0.04044999927282333f)
    {
        _S33 = x_6 * 0.07739938050508499f;
    }
    else
    {
        _S33 = (F32_pow((0.94786733388900757f * (x_6 + 0.05499999970197678f)), (2.40000009536743164f)));
    }
    return _S33;
}

inline __device__ float srgb_to_linear_rgb_grad(float x_7)
{
    float _S34;
    if(x_7 < 0.04044999927282333f)
    {
        _S34 = 0.07739938050508499f;
    }
    else
    {
        DiffPair_float_0 _S35;
        (&_S35)->primal_0 = 0.94786733388900757f * (x_7 + 0.05499999970197678f);
        (&_S35)->differential_0 = 0.94786733388900757f;
        DiffPair_float_0 _S36;
        (&_S36)->primal_0 = 2.40000009536743164f;
        (&_S36)->differential_0 = 0.0f;
        DiffPair_float_0 _S37 = _d_pow_1(&_S35, &_S36);
        _S34 = _S37.differential_0;
    }
    return _S34;
}

inline __device__ float splat_dc_encode(float dc_0)
{
    return 2.0f * (F32_log(((F32_max((0.564189612865448f * dc_0 + 1.0f), (9.999999960041972e-13f))))));
}

inline __device__ float splat_dc_decode(float x_8)
{
    return ((F32_exp((0.5f * x_8))) - 1.0f) * 1.77245378494262695f;
}

struct DiffPair_matrixx3Cfloatx2C3x2C3x3E_0
{
    Matrix<float, 3, 3>  primal_0;
    Matrix<float, 3, 3>  differential_0;
};

inline __device__ void _d_mul_0(DiffPair_matrixx3Cfloatx2C3x2C3x3E_0 * left_0, DiffPair_vectorx3Cfloatx2C3x3E_0 * right_0, float3  dOut_3)
{
    float _S38 = (*left_0).primal_0.rows[int(0)].x * dOut_3.x;
    Matrix<float, 3, 3>  left_d_result_0;
    *&(((&left_d_result_0)->rows + (int(0)))->x) = (*right_0).primal_0.x * dOut_3.x;
    float sum_0 = _S38 + (*left_0).primal_0.rows[int(1)].x * dOut_3.y;
    *&(((&left_d_result_0)->rows + (int(1)))->x) = (*right_0).primal_0.x * dOut_3.y;
    float sum_1 = sum_0 + (*left_0).primal_0.rows[int(2)].x * dOut_3.z;
    *&(((&left_d_result_0)->rows + (int(2)))->x) = (*right_0).primal_0.x * dOut_3.z;
    float3  right_d_result_0;
    *&((&right_d_result_0)->x) = sum_1;
    float _S39 = (*left_0).primal_0.rows[int(0)].y * dOut_3.x;
    *&(((&left_d_result_0)->rows + (int(0)))->y) = (*right_0).primal_0.y * dOut_3.x;
    float sum_2 = _S39 + (*left_0).primal_0.rows[int(1)].y * dOut_3.y;
    *&(((&left_d_result_0)->rows + (int(1)))->y) = (*right_0).primal_0.y * dOut_3.y;
    float sum_3 = sum_2 + (*left_0).primal_0.rows[int(2)].y * dOut_3.z;
    *&(((&left_d_result_0)->rows + (int(2)))->y) = (*right_0).primal_0.y * dOut_3.z;
    *&((&right_d_result_0)->y) = sum_3;
    float _S40 = (*left_0).primal_0.rows[int(0)].z * dOut_3.x;
    *&(((&left_d_result_0)->rows + (int(0)))->z) = (*right_0).primal_0.z * dOut_3.x;
    float sum_4 = _S40 + (*left_0).primal_0.rows[int(1)].z * dOut_3.y;
    *&(((&left_d_result_0)->rows + (int(1)))->z) = (*right_0).primal_0.z * dOut_3.y;
    float sum_5 = sum_4 + (*left_0).primal_0.rows[int(2)].z * dOut_3.z;
    *&(((&left_d_result_0)->rows + (int(2)))->z) = (*right_0).primal_0.z * dOut_3.z;
    *&((&right_d_result_0)->z) = sum_5;
    left_0->primal_0 = (*left_0).primal_0;
    left_0->differential_0 = left_d_result_0;
    right_0->primal_0 = (*right_0).primal_0;
    right_0->differential_0 = right_d_result_0;
    return;
}

inline __device__ float3  mul_0(Matrix<float, 3, 3>  left_1, float3  right_1)
{
    float3  result_6;
    int i_4 = int(0);
    for(;;)
    {
        if(i_4 < int(3))
        {
        }
        else
        {
            break;
        }
        int j_0 = int(0);
        float sum_6 = 0.0f;
        for(;;)
        {
            if(j_0 < int(3))
            {
            }
            else
            {
                break;
            }
            float sum_7 = sum_6 + _slang_vector_get_element(left_1.rows[i_4], j_0) * _slang_vector_get_element(right_1, j_0);
            j_0 = j_0 + int(1);
            sum_6 = sum_7;
        }
        *_slang_vector_get_element_ptr(&result_6, i_4) = sum_6;
        i_4 = i_4 + int(1);
    }
    return result_6;
}

inline __device__ void xfer_pass_grad_0(DiffPair_float_0 * dp_0, float v_out_1)
{
    dp_0->primal_0 = (*dp_0).primal_0;
    dp_0->differential_0 = v_out_1;
    return;
}

inline __device__ float xfer_max0_0(float x_9)
{
    return (F32_max((x_9), (0.0f)));
}

inline __device__ float xfer_filmic_0(float x_10)
{
    float t_0 = xfer_max0_0(x_10 - 0.00400000018998981f);
    float _S41 = 6.19999980926513672f * t_0;
    return t_0 * (_S41 + 0.5f) / (t_0 * (_S41 + 1.70000004768371582f) + 0.05999999865889549f);
}

inline __device__ float xfer_aces_0(float x_11)
{
    return x_11 * (2.50999999046325684f * x_11 + 0.02999999932944775f) / (x_11 * (2.43000006675720215f * x_11 + 0.5899999737739563f) + 0.14000000059604645f);
}

inline __device__ float clamp_0(float x_12, float minBound_0, float maxBound_0)
{
    return (F32_min(((F32_max((x_12), (minBound_0)))), (maxBound_0)));
}

inline __device__ float xfer_clamp01_0(float x_13)
{
    return clamp_0(x_13, 0.0f, 1.0f);
}

inline __device__ float xfer_hable_0(float x_14)
{
    float _S42 = 0.15000000596046448f * x_14;
    return (x_14 * (_S42 + 0.05000000074505806f) + 0.00400000018998981f) / (x_14 * (_S42 + 0.5f) + 0.06000000238418579f) - 0.06666666269302368f;
}

inline __device__ float xfer_uncharted2_0(float x_15)
{
    return xfer_hable_0(xfer_max0_0(x_15)) / xfer_hable_0(11.19999980926513672f);
}

inline __device__ float tone_encode_0(float x_16, int transfer_0)
{
    if(transfer_0 == int(3))
    {
        return xfer_filmic_0(x_16);
    }
    float _S43;
    if(transfer_0 == int(2))
    {
        float _S44 = xfer_clamp01_0(xfer_aces_0(xfer_max0_0(x_16)));
        if(_S44 < 0.00313080009073019f)
        {
            _S43 = _S44 * 12.92000007629394531f;
        }
        else
        {
            _S43 = 1.0549999475479126f * (F32_pow((_S44), (0.4166666567325592f))) - 0.05499999970197678f;
        }
        return _S43;
    }
    if(transfer_0 == int(4))
    {
        float _S45 = xfer_clamp01_0(xfer_uncharted2_0(x_16));
        if(_S45 < 0.00313080009073019f)
        {
            _S43 = _S45 * 12.92000007629394531f;
        }
        else
        {
            _S43 = 1.0549999475479126f * (F32_pow((_S45), (0.4166666567325592f))) - 0.05499999970197678f;
        }
        return _S43;
    }
    if(transfer_0 == int(1))
    {
        float _S46 = xfer_clamp01_0(x_16);
        if(_S46 < 0.00313080009073019f)
        {
            _S43 = _S46 * 12.92000007629394531f;
        }
        else
        {
            _S43 = 1.0549999475479126f * (F32_pow((_S46), (0.4166666567325592f))) - 0.05499999970197678f;
        }
        return _S43;
    }
    if(x_16 < 0.00313080009073019f)
    {
        _S43 = x_16 * 12.92000007629394531f;
    }
    else
    {
        _S43 = 1.0549999475479126f * (F32_pow((x_16), (0.4166666567325592f))) - 0.05499999970197678f;
    }
    return _S43;
}

inline __device__ float3  working_to_display(float3  rgb_2, Matrix<float, 3, 3>  color_matrix_0, int transfer_1, bool is_linear_0)
{
    float3  _S47;
    if(!is_linear_0)
    {
        float _S48 = rgb_2.x;
        float _S49;
        if(_S48 < 0.04044999927282333f)
        {
            _S49 = _S48 * 0.07739938050508499f;
        }
        else
        {
            _S49 = (F32_pow((0.94786733388900757f * (_S48 + 0.05499999970197678f)), (2.40000009536743164f)));
        }
        float _S50 = rgb_2.y;
        float _S51;
        if(_S50 < 0.04044999927282333f)
        {
            _S51 = _S50 * 0.07739938050508499f;
        }
        else
        {
            _S51 = (F32_pow((0.94786733388900757f * (_S50 + 0.05499999970197678f)), (2.40000009536743164f)));
        }
        float _S52 = rgb_2.z;
        float _S53;
        if(_S52 < 0.04044999927282333f)
        {
            _S53 = _S52 * 0.07739938050508499f;
        }
        else
        {
            _S53 = (F32_pow((0.94786733388900757f * (_S52 + 0.05499999970197678f)), (2.40000009536743164f)));
        }
        _S47 = make_float3 (_S49, _S51, _S53);
    }
    else
    {
        _S47 = rgb_2;
    }
    float3  _S54 = mul_0(color_matrix_0, _S47);
    return make_float3 (tone_encode_0(_S54.x, transfer_1), tone_encode_0(_S54.y, transfer_1), tone_encode_0(_S54.z, transfer_1));
}

inline __device__ float s_primal_ctx_pow_0(float _S55, float _S56)
{
    return (F32_pow((_S55), (_S56)));
}

inline __device__ float3  s_primal_ctx_mul_0(Matrix<float, 3, 3>  _S57, float3  _S58)
{
    return mul_0(_S57, _S58);
}

inline __device__ float s_primal_ctx_xfer_max0_0(float _S59)
{
    return xfer_max0_0(_S59);
}

inline __device__ float s_primal_ctx_xfer_aces_0(float dpx_4)
{
    return dpx_4 * (2.50999999046325684f * dpx_4 + 0.02999999932944775f) / (dpx_4 * (2.43000006675720215f * dpx_4 + 0.5899999737739563f) + 0.14000000059604645f);
}

inline __device__ float s_primal_ctx_xfer_clamp01_0(float _S60)
{
    return xfer_clamp01_0(_S60);
}

inline __device__ float s_primal_ctx_xfer_hable_0(float dpx_5)
{
    float _S61 = 0.15000000596046448f * dpx_5;
    return (dpx_5 * (_S61 + 0.05000000074505806f) + 0.00400000018998981f) / (dpx_5 * (_S61 + 0.5f) + 0.06000000238418579f) - 0.06666666269302368f;
}

inline __device__ float s_primal_ctx_xfer_uncharted2_0(float dpx_6)
{
    return s_primal_ctx_xfer_hable_0(s_primal_ctx_xfer_max0_0(dpx_6)) / s_primal_ctx_xfer_hable_0(11.19999980926513672f);
}

inline __device__ void s_bwd_prop_pow_0(DiffPair_float_0 * _S62, DiffPair_float_0 * _S63, float _S64)
{
    _d_pow_0(_S62, _S63, _S64);
    return;
}

inline __device__ void s_bwd_prop_xfer_clamp01_0(DiffPair_float_0 * _S65, float _S66)
{
    xfer_pass_grad_0(_S65, _S66);
    return;
}

inline __device__ void s_bwd_prop_xfer_hable_0(DiffPair_float_0 * dpx_7, float _s_dOut_1)
{
    float _S67 = 0.15000000596046448f * (*dpx_7).primal_0;
    float _S68 = _S67 + 0.05000000074505806f;
    float _S69 = _S67 + 0.5f;
    float _S70 = (*dpx_7).primal_0 * _S69 + 0.06000000238418579f;
    float _S71 = _s_dOut_1 / (_S70 * _S70);
    float _S72 = ((*dpx_7).primal_0 * _S68 + 0.00400000018998981f) * - _S71;
    float _S73 = _S70 * _S71;
    float _S74 = _S69 * _S72 + _S68 * _S73 + 0.15000000596046448f * ((*dpx_7).primal_0 * _S72 + (*dpx_7).primal_0 * _S73);
    dpx_7->primal_0 = (*dpx_7).primal_0;
    dpx_7->differential_0 = _S74;
    return;
}

inline __device__ void s_bwd_prop_xfer_max0_0(DiffPair_float_0 * _S75, float _S76)
{
    xfer_pass_grad_0(_S75, _S76);
    return;
}

inline __device__ void s_bwd_prop_xfer_uncharted2_0(DiffPair_float_0 * dpx_8, float _s_dOut_2)
{
    float _S77 = s_primal_ctx_xfer_hable_0(11.19999980926513672f);
    float _S78 = _S77 * (_s_dOut_2 / (_S77 * _S77));
    DiffPair_float_0 _S79;
    (&_S79)->primal_0 = s_primal_ctx_xfer_max0_0((*dpx_8).primal_0);
    (&_S79)->differential_0 = 0.0f;
    s_bwd_prop_xfer_hable_0(&_S79, _S78);
    DiffPair_float_0 _S80;
    (&_S80)->primal_0 = (*dpx_8).primal_0;
    (&_S80)->differential_0 = 0.0f;
    s_bwd_prop_xfer_max0_0(&_S80, _S79.differential_0);
    dpx_8->primal_0 = (*dpx_8).primal_0;
    dpx_8->differential_0 = _S80.differential_0;
    return;
}

inline __device__ void s_bwd_prop_xfer_aces_0(DiffPair_float_0 * dpx_9, float _s_dOut_3)
{
    float _S81 = 2.50999999046325684f * (*dpx_9).primal_0 + 0.02999999932944775f;
    float _S82 = 2.43000006675720215f * (*dpx_9).primal_0 + 0.5899999737739563f;
    float _S83 = (*dpx_9).primal_0 * _S82 + 0.14000000059604645f;
    float _S84 = _s_dOut_3 / (_S83 * _S83);
    float _S85 = (*dpx_9).primal_0 * _S81 * - _S84;
    float _S86 = _S83 * _S84;
    float _S87 = _S82 * _S85 + 2.43000006675720215f * ((*dpx_9).primal_0 * _S85) + _S81 * _S86 + 2.50999999046325684f * ((*dpx_9).primal_0 * _S86);
    dpx_9->primal_0 = (*dpx_9).primal_0;
    dpx_9->differential_0 = _S87;
    return;
}

inline __device__ void s_bwd_prop_xfer_filmic_0(DiffPair_float_0 * dpx_10, float _s_dOut_4)
{
    float _S88 = (*dpx_10).primal_0 - 0.00400000018998981f;
    float _S89 = s_primal_ctx_xfer_max0_0(_S88);
    float _S90 = 6.19999980926513672f * _S89;
    float _S91 = _S90 + 0.5f;
    float _S92 = _S90 + 1.70000004768371582f;
    float _S93 = _S89 * _S92 + 0.05999999865889549f;
    float _S94 = _s_dOut_4 / (_S93 * _S93);
    float _S95 = _S89 * _S91 * - _S94;
    float _S96 = _S93 * _S94;
    float _S97 = _S92 * _S95 + _S91 * _S96 + 6.19999980926513672f * (_S89 * _S95 + _S89 * _S96);
    DiffPair_float_0 _S98;
    (&_S98)->primal_0 = _S88;
    (&_S98)->differential_0 = 0.0f;
    s_bwd_prop_xfer_max0_0(&_S98, _S97);
    dpx_10->primal_0 = (*dpx_10).primal_0;
    dpx_10->differential_0 = _S98.differential_0;
    return;
}

inline __device__ void s_bwd_prop_tone_encode_0(DiffPair_float_0 * dpx_11, int transfer_2, float _s_dOut_5)
{
    DiffPair_float_0 _S99 = *dpx_11;
    bool _S100 = transfer_2 == int(3);
    bool _S101 = !_S100;
    bool _runFlag_0;
    bool _runFlag_1;
    bool _runFlag_2;
    bool _S102;
    bool _S103;
    bool _S104;
    float _S105;
    float _S106;
    float _S107;
    float _S108;
    float _S109;
    float _S110;
    if(_S101)
    {
        bool _S111 = transfer_2 == int(2);
        if(_S111)
        {
            float _S112 = s_primal_ctx_xfer_max0_0(_S99.primal_0);
            float _S113 = s_primal_ctx_xfer_aces_0(_S112);
            float _S114 = s_primal_ctx_xfer_clamp01_0(_S113);
            _runFlag_0 = false;
            _S105 = _S114;
            _S106 = _S113;
            _S107 = _S112;
        }
        else
        {
            _runFlag_0 = _S101;
            _S105 = 0.0f;
            _S106 = 0.0f;
            _S107 = 0.0f;
        }
        if(_runFlag_0)
        {
            bool _S115 = transfer_2 == int(4);
            if(_S115)
            {
                float _S116 = s_primal_ctx_xfer_uncharted2_0(_S99.primal_0);
                float _S117 = s_primal_ctx_xfer_clamp01_0(_S116);
                _runFlag_1 = false;
                _S108 = _S117;
                _S109 = _S116;
            }
            else
            {
                _runFlag_1 = _runFlag_0;
                _S108 = 0.0f;
                _S109 = 0.0f;
            }
            if(_runFlag_1)
            {
                bool _S118 = transfer_2 == int(1);
                if(_S118)
                {
                    float _S119 = s_primal_ctx_xfer_clamp01_0(_S99.primal_0);
                    _runFlag_2 = false;
                    _S110 = _S119;
                }
                else
                {
                    _runFlag_2 = _runFlag_1;
                    _S110 = 0.0f;
                }
                _S102 = _S118;
            }
            else
            {
                _runFlag_2 = false;
                _S102 = false;
                _S110 = 0.0f;
            }
            float _S120 = _S108;
            float _S121 = _S109;
            _S108 = _S110;
            _S103 = _S115;
            _S109 = _S120;
            _S110 = _S121;
        }
        else
        {
            _runFlag_1 = false;
            _runFlag_2 = false;
            _S102 = false;
            _S108 = 0.0f;
            _S103 = false;
            _S109 = 0.0f;
            _S110 = 0.0f;
        }
        float _S122 = _S105;
        float _S123 = _S106;
        float _S124 = _S107;
        _S105 = _S108;
        _S106 = _S109;
        _S107 = _S110;
        _S104 = _S111;
        _S108 = _S122;
        _S109 = _S123;
        _S110 = _S124;
    }
    else
    {
        _runFlag_0 = false;
        _runFlag_1 = false;
        _runFlag_2 = false;
        _S102 = false;
        _S105 = 0.0f;
        _S103 = false;
        _S106 = 0.0f;
        _S107 = 0.0f;
        _S104 = false;
        _S108 = 0.0f;
        _S109 = 0.0f;
        _S110 = 0.0f;
    }
    if(_S101)
    {
        if(_runFlag_0)
        {
            float _S125;
            if(_runFlag_1)
            {
                float _S126;
                if(_runFlag_2)
                {
                    if((_S99.primal_0) < 0.00313080009073019f)
                    {
                        _S125 = 12.92000007629394531f * _s_dOut_5;
                    }
                    else
                    {
                        float _S127 = 1.0549999475479126f * _s_dOut_5;
                        DiffPair_float_0 _S128;
                        (&_S128)->primal_0 = _S99.primal_0;
                        (&_S128)->differential_0 = 0.0f;
                        DiffPair_float_0 _S129;
                        (&_S129)->primal_0 = 0.4166666567325592f;
                        (&_S129)->differential_0 = 0.0f;
                        s_bwd_prop_pow_0(&_S128, &_S129, _S127);
                        _S125 = _S128.differential_0;
                    }
                    float _S130 = _S125;
                    _S125 = 0.0f;
                    _S126 = _S130;
                }
                else
                {
                    _S125 = _s_dOut_5;
                    _S126 = 0.0f;
                }
                if(_S102)
                {
                    if(_S105 < 0.00313080009073019f)
                    {
                        _S105 = 12.92000007629394531f * _S125;
                    }
                    else
                    {
                        float _S131 = 1.0549999475479126f * _S125;
                        DiffPair_float_0 _S132;
                        (&_S132)->primal_0 = _S105;
                        (&_S132)->differential_0 = 0.0f;
                        DiffPair_float_0 _S133;
                        (&_S133)->primal_0 = 0.4166666567325592f;
                        (&_S133)->differential_0 = 0.0f;
                        s_bwd_prop_pow_0(&_S132, &_S133, _S131);
                        _S105 = _S132.differential_0;
                    }
                    DiffPair_float_0 _S134;
                    (&_S134)->primal_0 = _S99.primal_0;
                    (&_S134)->differential_0 = 0.0f;
                    s_bwd_prop_xfer_clamp01_0(&_S134, _S105);
                    float _S135 = _S134.differential_0 + _S126;
                    _S105 = 0.0f;
                    _S125 = _S135;
                }
                else
                {
                    _S105 = _S125;
                    _S125 = _S126;
                }
            }
            else
            {
                _S105 = _s_dOut_5;
                _S125 = 0.0f;
            }
            if(_S103)
            {
                if(_S106 < 0.00313080009073019f)
                {
                    _S105 = 12.92000007629394531f * _S105;
                }
                else
                {
                    float _S136 = 1.0549999475479126f * _S105;
                    DiffPair_float_0 _S137;
                    (&_S137)->primal_0 = _S106;
                    (&_S137)->differential_0 = 0.0f;
                    DiffPair_float_0 _S138;
                    (&_S138)->primal_0 = 0.4166666567325592f;
                    (&_S138)->differential_0 = 0.0f;
                    s_bwd_prop_pow_0(&_S137, &_S138, _S136);
                    _S105 = _S137.differential_0;
                }
                DiffPair_float_0 _S139;
                (&_S139)->primal_0 = _S107;
                (&_S139)->differential_0 = 0.0f;
                s_bwd_prop_xfer_clamp01_0(&_S139, _S105);
                DiffPair_float_0 _S140;
                (&_S140)->primal_0 = _S99.primal_0;
                (&_S140)->differential_0 = 0.0f;
                s_bwd_prop_xfer_uncharted2_0(&_S140, _S139.differential_0);
                float _S141 = _S140.differential_0 + _S125;
                _S105 = 0.0f;
                _S106 = _S141;
            }
            else
            {
                _S106 = _S125;
            }
        }
        else
        {
            _S105 = _s_dOut_5;
            _S106 = 0.0f;
        }
        if(_S104)
        {
            if(_S108 < 0.00313080009073019f)
            {
                _S105 = 12.92000007629394531f * _S105;
            }
            else
            {
                float _S142 = 1.0549999475479126f * _S105;
                DiffPair_float_0 _S143;
                (&_S143)->primal_0 = _S108;
                (&_S143)->differential_0 = 0.0f;
                DiffPair_float_0 _S144;
                (&_S144)->primal_0 = 0.4166666567325592f;
                (&_S144)->differential_0 = 0.0f;
                s_bwd_prop_pow_0(&_S143, &_S144, _S142);
                _S105 = _S143.differential_0;
            }
            DiffPair_float_0 _S145;
            (&_S145)->primal_0 = _S109;
            (&_S145)->differential_0 = 0.0f;
            s_bwd_prop_xfer_clamp01_0(&_S145, _S105);
            DiffPair_float_0 _S146;
            (&_S146)->primal_0 = _S110;
            (&_S146)->differential_0 = 0.0f;
            s_bwd_prop_xfer_aces_0(&_S146, _S145.differential_0);
            DiffPair_float_0 _S147;
            (&_S147)->primal_0 = _S99.primal_0;
            (&_S147)->differential_0 = 0.0f;
            s_bwd_prop_xfer_max0_0(&_S147, _S146.differential_0);
            float _S148 = _S147.differential_0 + _S106;
            _S105 = 0.0f;
            _S106 = _S148;
        }
    }
    else
    {
        _S105 = _s_dOut_5;
        _S106 = 0.0f;
    }
    if(_S100)
    {
        DiffPair_float_0 _S149;
        (&_S149)->primal_0 = _S99.primal_0;
        (&_S149)->differential_0 = 0.0f;
        s_bwd_prop_xfer_filmic_0(&_S149, _S105);
        _S105 = _S149.differential_0 + _S106;
    }
    else
    {
        _S105 = _S106;
    }
    dpx_11->primal_0 = (*dpx_11).primal_0;
    dpx_11->differential_0 = _S105;
    return;
}

inline __device__ void s_bwd_prop_mul_0(DiffPair_matrixx3Cfloatx2C3x2C3x3E_0 * _S150, DiffPair_vectorx3Cfloatx2C3x3E_0 * _S151, float3  _S152)
{
    _d_mul_0(_S150, _S151, _S152);
    return;
}

inline __device__ void s_bwd_prop_working_to_display_0(DiffPair_vectorx3Cfloatx2C3x3E_0 * dprgb_0, Matrix<float, 3, 3>  color_matrix_1, int transfer_3, bool is_linear_1, float3  _s_dOut_6)
{
    DiffPair_vectorx3Cfloatx2C3x3E_0 _S153 = *dprgb_0;
    bool _S154 = !is_linear_1;
    float _S155;
    float _S156;
    float _S157;
    float3  _S158;
    if(_S154)
    {
        float _S159 = _S153.primal_0.x;
        if(_S159 < 0.04044999927282333f)
        {
            _S155 = _S159 * 0.07739938050508499f;
        }
        else
        {
            _S155 = s_primal_ctx_pow_0(0.94786733388900757f * (_S159 + 0.05499999970197678f), 2.40000009536743164f);
        }
        float _S160 = _S153.primal_0.y;
        if(_S160 < 0.04044999927282333f)
        {
            _S156 = _S160 * 0.07739938050508499f;
        }
        else
        {
            _S156 = s_primal_ctx_pow_0(0.94786733388900757f * (_S160 + 0.05499999970197678f), 2.40000009536743164f);
        }
        float _S161 = _S153.primal_0.z;
        if(_S161 < 0.04044999927282333f)
        {
            _S157 = _S161 * 0.07739938050508499f;
        }
        else
        {
            _S157 = s_primal_ctx_pow_0(0.94786733388900757f * (_S161 + 0.05499999970197678f), 2.40000009536743164f);
        }
        _S158 = make_float3 (_S155, _S156, _S157);
        _S155 = _S161;
        _S156 = _S160;
        _S157 = _S159;
    }
    else
    {
        _S158 = _S153.primal_0;
        _S155 = 0.0f;
        _S156 = 0.0f;
        _S157 = 0.0f;
    }
    float3  _S162 = s_primal_ctx_mul_0(color_matrix_1, _S158);
    float _S163 = _S162.x;
    float _S164 = _S162.y;
    float _S165 = _S162.z;
    DiffPair_float_0 _S166;
    (&_S166)->primal_0 = _S165;
    (&_S166)->differential_0 = 0.0f;
    s_bwd_prop_tone_encode_0(&_S166, transfer_3, _s_dOut_6.z);
    DiffPair_float_0 _S167;
    (&_S167)->primal_0 = _S164;
    (&_S167)->differential_0 = 0.0f;
    s_bwd_prop_tone_encode_0(&_S167, transfer_3, _s_dOut_6.y);
    DiffPair_float_0 _S168;
    (&_S168)->primal_0 = _S163;
    (&_S168)->differential_0 = 0.0f;
    s_bwd_prop_tone_encode_0(&_S168, transfer_3, _s_dOut_6.x);
    float3  _S169 = make_float3 (_S168.differential_0, _S167.differential_0, _S166.differential_0);
    Matrix<float, 3, 3>  _S170 = makeMatrix<float, 3, 3> (0.0f);
    DiffPair_matrixx3Cfloatx2C3x2C3x3E_0 _S171;
    (&_S171)->primal_0 = color_matrix_1;
    (&_S171)->differential_0 = _S170;
    float3  _S172 = make_float3 (0.0f);
    DiffPair_vectorx3Cfloatx2C3x3E_0 _S173;
    (&_S173)->primal_0 = _S158;
    (&_S173)->differential_0 = _S172;
    s_bwd_prop_mul_0(&_S171, &_S173, _S169);
    DiffPair_vectorx3Cfloatx2C3x3E_0 _S174 = _S173;
    if(_S154)
    {
        bool _S175 = _S155 < 0.04044999927282333f;
        if(_S175)
        {
            _S155 = 0.0f;
        }
        else
        {
            _S155 = 0.94786733388900757f * (_S155 + 0.05499999970197678f);
        }
        if(_S175)
        {
            _S155 = 0.07739938050508499f * _S174.differential_0.z;
        }
        else
        {
            DiffPair_float_0 _S176;
            (&_S176)->primal_0 = _S155;
            (&_S176)->differential_0 = 0.0f;
            DiffPair_float_0 _S177;
            (&_S177)->primal_0 = 2.40000009536743164f;
            (&_S177)->differential_0 = 0.0f;
            s_bwd_prop_pow_0(&_S176, &_S177, _S174.differential_0.z);
            _S155 = 0.94786733388900757f * _S176.differential_0;
        }
        bool _S178 = _S156 < 0.04044999927282333f;
        if(_S178)
        {
            _S156 = 0.0f;
        }
        else
        {
            _S156 = 0.94786733388900757f * (_S156 + 0.05499999970197678f);
        }
        if(_S178)
        {
            _S156 = 0.07739938050508499f * _S174.differential_0.y;
        }
        else
        {
            DiffPair_float_0 _S179;
            (&_S179)->primal_0 = _S156;
            (&_S179)->differential_0 = 0.0f;
            DiffPair_float_0 _S180;
            (&_S180)->primal_0 = 2.40000009536743164f;
            (&_S180)->differential_0 = 0.0f;
            s_bwd_prop_pow_0(&_S179, &_S180, _S174.differential_0.y);
            _S156 = 0.94786733388900757f * _S179.differential_0;
        }
        bool _S181 = _S157 < 0.04044999927282333f;
        if(_S181)
        {
            _S157 = 0.0f;
        }
        else
        {
            _S157 = 0.94786733388900757f * (_S157 + 0.05499999970197678f);
        }
        if(_S181)
        {
            _S157 = 0.07739938050508499f * _S174.differential_0.x;
        }
        else
        {
            DiffPair_float_0 _S182;
            (&_S182)->primal_0 = _S157;
            (&_S182)->differential_0 = 0.0f;
            DiffPair_float_0 _S183;
            (&_S183)->primal_0 = 2.40000009536743164f;
            (&_S183)->differential_0 = 0.0f;
            s_bwd_prop_pow_0(&_S182, &_S183, _S174.differential_0.x);
            _S157 = 0.94786733388900757f * _S182.differential_0;
        }
        _S158 = make_float3 (_S157, _S156, _S155);
    }
    else
    {
        _S158 = _S174.differential_0;
    }
    dprgb_0->primal_0 = (*dprgb_0).primal_0;
    dprgb_0->differential_0 = _S158;
    return;
}

inline __device__ void s_bwd_working_to_display_0(DiffPair_vectorx3Cfloatx2C3x3E_0 * _S184, Matrix<float, 3, 3>  _S185, int _S186, bool _S187, float3  _S188)
{
    s_bwd_prop_working_to_display_0(_S184, _S185, _S186, _S187, _S188);
    return;
}

inline __device__ float3  working_to_display_bwd(float3  rgb_3, Matrix<float, 3, 3>  color_matrix_2, int transfer_4, bool is_linear_2, float3  v_out_rgb_1)
{
    float3  _S189 = make_float3 (0.0f);
    DiffPair_vectorx3Cfloatx2C3x3E_0 p_rgb_1;
    (&p_rgb_1)->primal_0 = rgb_3;
    (&p_rgb_1)->differential_0 = _S189;
    s_bwd_working_to_display_0(&p_rgb_1, color_matrix_2, transfer_4, is_linear_2, v_out_rgb_1);
    return p_rgb_1.differential_0;
}

inline __device__ void _d_sqrt_0(DiffPair_float_0 * dpx_12, float dOut_4)
{
    float _S190 = 0.5f / (F32_sqrt(((F32_max((1.00000001168609742e-07f), ((*dpx_12).primal_0)))))) * dOut_4;
    dpx_12->primal_0 = (*dpx_12).primal_0;
    dpx_12->differential_0 = _S190;
    return;
}

inline __device__ float xfer_filmic_inv_0(float y_4)
{
    float _S191 = (F32_min((y_4), (xfer_filmic_0(11.19999980926513672f))));
    float a_0 = 6.19999980926513672f * (1.0f - _S191);
    float b_0 = 0.5f - 1.70000004768371582f * _S191;
    return (- b_0 + (F32_sqrt(((F32_max((b_0 * b_0 - 4.0f * a_0 * (-0.05999999865889549f * _S191)), (0.0f))))))) / (2.0f * a_0) + 0.00400000018998981f;
}

inline __device__ float xfer_aces_inv_0(float y_5)
{
    float a_1 = 2.50999999046325684f - 2.43000006675720215f * y_5;
    float b_1 = 0.02999999932944775f - 0.5899999737739563f * y_5;
    return (- b_1 + (F32_sqrt(((F32_max((b_1 * b_1 - 4.0f * a_1 * (-0.14000000059604645f * y_5)), (0.0f))))))) / (2.0f * a_1);
}

inline __device__ float xfer_uncharted2_inv_0(float y_6)
{
    float r_0 = y_6 * xfer_hable_0(11.19999980926513672f) + 0.06666666269302368f;
    float a_2 = 0.15000000596046448f * (1.0f - r_0);
    float b_2 = 0.5f * (0.10000000149011612f - r_0);
    return (- b_2 + (F32_sqrt(((F32_max((b_2 * b_2 - 4.0f * a_2 * (0.20000000298023224f * (0.01999999955296516f - r_0 * 0.30000001192092896f))), (0.0f))))))) / (2.0f * a_2);
}

inline __device__ float tone_decode_0(float d_0, int transfer_5)
{
    if(transfer_5 == int(3))
    {
        return xfer_filmic_inv_0(d_0);
    }
    float _S192;
    if(transfer_5 == int(2))
    {
        if(d_0 < 0.04044999927282333f)
        {
            _S192 = d_0 * 0.07739938050508499f;
        }
        else
        {
            _S192 = (F32_pow((0.94786733388900757f * (d_0 + 0.05499999970197678f)), (2.40000009536743164f)));
        }
        return xfer_aces_inv_0(_S192);
    }
    if(transfer_5 == int(4))
    {
        if(d_0 < 0.04044999927282333f)
        {
            _S192 = d_0 * 0.07739938050508499f;
        }
        else
        {
            _S192 = (F32_pow((0.94786733388900757f * (d_0 + 0.05499999970197678f)), (2.40000009536743164f)));
        }
        return xfer_uncharted2_inv_0(_S192);
    }
    if(d_0 < 0.04044999927282333f)
    {
        _S192 = d_0 * 0.07739938050508499f;
    }
    else
    {
        _S192 = (F32_pow((0.94786733388900757f * (d_0 + 0.05499999970197678f)), (2.40000009536743164f)));
    }
    return _S192;
}

inline __device__ float3  display_to_working3(float3  rgb_4, int transfer_6, bool is_linear_3)
{
    float _S193 = tone_decode_0(rgb_4.x, transfer_6);
    float _S194 = tone_decode_0(rgb_4.y, transfer_6);
    float _S195 = tone_decode_0(rgb_4.z, transfer_6);
    float3  lin_0 = make_float3 (_S193, _S194, _S195);
    if(is_linear_3)
    {
        return lin_0;
    }
    float _S196;
    if(_S193 < 0.00313080009073019f)
    {
        _S196 = _S193 * 12.92000007629394531f;
    }
    else
    {
        _S196 = 1.0549999475479126f * (F32_pow((_S193), (0.4166666567325592f))) - 0.05499999970197678f;
    }
    float _S197;
    if(_S194 < 0.00313080009073019f)
    {
        _S197 = _S194 * 12.92000007629394531f;
    }
    else
    {
        _S197 = 1.0549999475479126f * (F32_pow((_S194), (0.4166666567325592f))) - 0.05499999970197678f;
    }
    float _S198;
    if(_S195 < 0.00313080009073019f)
    {
        _S198 = _S195 * 12.92000007629394531f;
    }
    else
    {
        _S198 = 1.0549999475479126f * (F32_pow((_S195), (0.4166666567325592f))) - 0.05499999970197678f;
    }
    return make_float3 (_S196, _S197, _S198);
}

inline __device__ void _d_cross_0(DiffPair_vectorx3Cfloatx2C3x3E_0 * a_3, DiffPair_vectorx3Cfloatx2C3x3E_0 * b_3, float3  dOut_5)
{
    float _S199 = dOut_5.y;
    float _S200 = dOut_5.z;
    float _S201 = dOut_5.x;
    float _S202 = (*a_3).primal_0.z * _S199 + - (*a_3).primal_0.y * _S200;
    float _S203 = - (*a_3).primal_0.z * _S201 + (*a_3).primal_0.x * _S200;
    float _S204 = (*a_3).primal_0.y * _S201 + - (*a_3).primal_0.x * _S199;
    float3  _S205 = make_float3 (- (*b_3).primal_0.z * _S199 + (*b_3).primal_0.y * _S200, (*b_3).primal_0.z * _S201 + - (*b_3).primal_0.x * _S200, - (*b_3).primal_0.y * _S201 + (*b_3).primal_0.x * _S199);
    a_3->primal_0 = (*a_3).primal_0;
    a_3->differential_0 = _S205;
    float3  _S206 = make_float3 (_S202, _S203, _S204);
    b_3->primal_0 = (*b_3).primal_0;
    b_3->differential_0 = _S206;
    return;
}

inline __device__ float3  cross_0(float3  left_2, float3  right_2)
{
    float _S207 = left_2.y;
    float _S208 = right_2.z;
    float _S209 = left_2.z;
    float _S210 = right_2.y;
    float _S211 = right_2.x;
    float _S212 = left_2.x;
    return make_float3 (_S207 * _S208 - _S209 * _S210, _S209 * _S211 - _S212 * _S208, _S212 * _S210 - _S207 * _S211);
}

inline __device__ float length_0(float3  x_17)
{
    return (F32_sqrt((dot_0(x_17, x_17))));
}

inline __device__ float length_1(float2  x_18)
{
    return (F32_sqrt((dot_1(x_18, x_18))));
}

inline __device__ float3  points_to_normal(FixedArray<float3 , 4>  points_0)
{
    float3  _S213 = points_0[int(0)];
    bool _S214;
    if((dot_0(_S213, _S213)) == 0.0f)
    {
        _S214 = true;
    }
    else
    {
        float3  _S215 = points_0[int(1)];
        _S214 = (dot_0(_S215, _S215)) == 0.0f;
    }
    if(_S214)
    {
        _S214 = true;
    }
    else
    {
        float3  _S216 = points_0[int(2)];
        _S214 = (dot_0(_S216, _S216)) == 0.0f;
    }
    if(_S214)
    {
        _S214 = true;
    }
    else
    {
        float3  _S217 = points_0[int(3)];
        _S214 = (dot_0(_S217, _S217)) == 0.0f;
    }
    if(_S214)
    {
        return make_float3 (0.0f);
    }
    float3  normal_0 = cross_0(points_0[int(1)] - points_0[int(0)], - (points_0[int(3)] - points_0[int(2)]));
    float3  normal_1;
    if((dot_0(normal_0, normal_0)) != 0.0f)
    {
        normal_1 = normal_0 / make_float3 (length_0(normal_0));
    }
    else
    {
        normal_1 = normal_0;
    }
    return normal_1;
}

struct DiffPair_arrayx3Cvectorx3Cfloatx2C3x3Ex2C4x3E_0
{
    FixedArray<float3 , 4>  primal_0;
    FixedArray<float3 , 4>  differential_0;
};

inline __device__ float s_primal_ctx_dot_0(float3  _S218, float3  _S219)
{
    return dot_0(_S218, _S219);
}

inline __device__ float3  s_primal_ctx_cross_0(float3  _S220, float3  _S221)
{
    return cross_0(_S220, _S221);
}

inline __device__ void s_bwd_prop_sqrt_0(DiffPair_float_0 * _S222, float _S223)
{
    _d_sqrt_0(_S222, _S223);
    return;
}

inline __device__ void s_bwd_prop_length_impl_0(DiffPair_vectorx3Cfloatx2C3x3E_0 * dpx_13, float _s_dOut_7)
{
    float _S224 = (*dpx_13).primal_0.x;
    float _S225 = (*dpx_13).primal_0.y;
    float _S226 = (*dpx_13).primal_0.z;
    DiffPair_float_0 _S227;
    (&_S227)->primal_0 = _S224 * _S224 + _S225 * _S225 + _S226 * _S226;
    (&_S227)->differential_0 = 0.0f;
    s_bwd_prop_sqrt_0(&_S227, _s_dOut_7);
    float _S228 = (*dpx_13).primal_0.z * _S227.differential_0;
    float _S229 = _S228 + _S228;
    float _S230 = (*dpx_13).primal_0.y * _S227.differential_0;
    float _S231 = _S230 + _S230;
    float _S232 = (*dpx_13).primal_0.x * _S227.differential_0;
    float _S233 = _S232 + _S232;
    float3  _S234 = make_float3 (0.0f);
    *&((&_S234)->z) = _S229;
    *&((&_S234)->y) = _S231;
    *&((&_S234)->x) = _S233;
    dpx_13->primal_0 = (*dpx_13).primal_0;
    dpx_13->differential_0 = _S234;
    return;
}

inline __device__ void s_bwd_length_impl_0(DiffPair_vectorx3Cfloatx2C3x3E_0 * _S235, float _S236)
{
    s_bwd_prop_length_impl_0(_S235, _S236);
    return;
}

inline __device__ void s_bwd_prop_dot_0(DiffPair_vectorx3Cfloatx2C3x3E_0 * _S237, DiffPair_vectorx3Cfloatx2C3x3E_0 * _S238, float _S239)
{
    _d_dot_0(_S237, _S238, _S239);
    return;
}

inline __device__ void s_bwd_prop_cross_0(DiffPair_vectorx3Cfloatx2C3x3E_0 * _S240, DiffPair_vectorx3Cfloatx2C3x3E_0 * _S241, float3  _S242)
{
    _d_cross_0(_S240, _S241, _S242);
    return;
}

inline __device__ void s_bwd_prop_points_to_normal_0(DiffPair_arrayx3Cvectorx3Cfloatx2C3x3Ex2C4x3E_0 * dppoints_0, float3  _s_dOut_8)
{
    FixedArray<float3 , 4>  _S243 = dppoints_0->primal_0;
    float3  _S244 = make_float3 (0.0f);
    float3  _S245 = dppoints_0->primal_0[int(0)];
    bool _S246 = (s_primal_ctx_dot_0(_S245, _S245)) == 0.0f;
    bool _S247;
    float3  _S248;
    if(_S246)
    {
        _S247 = true;
        _S248 = _S244;
    }
    else
    {
        float3  _S249 = _S243[int(1)];
        _S247 = (s_primal_ctx_dot_0(_S249, _S249)) == 0.0f;
        _S248 = _S243[int(1)];
    }
    bool _S250;
    float3  _S251;
    if(_S247)
    {
        _S250 = true;
        _S251 = _S244;
    }
    else
    {
        float3  _S252 = _S243[int(2)];
        _S250 = (s_primal_ctx_dot_0(_S252, _S252)) == 0.0f;
        _S251 = _S243[int(2)];
    }
    bool _S253;
    float3  _S254;
    if(_S250)
    {
        _S253 = true;
        _S254 = _S244;
    }
    else
    {
        float3  _S255 = _S243[int(3)];
        _S253 = (s_primal_ctx_dot_0(_S255, _S255)) == 0.0f;
        _S254 = _S243[int(3)];
    }
    bool _S256 = !_S253;
    float3  _S257;
    float3  _S258;
    float3  _S259;
    float3  _S260;
    float3  _S261;
    if(_S256)
    {
        float3  dx_0 = _S243[int(1)] - _S243[int(0)];
        float3  _S262 = - (_S243[int(3)] - _S243[int(2)]);
        float3  _S263 = s_primal_ctx_cross_0(dx_0, _S262);
        bool _S264 = (s_primal_ctx_dot_0(_S263, _S263)) != 0.0f;
        if(_S264)
        {
            float _S265 = length_0(_S263);
            float3  _S266 = make_float3 (_S265);
            _S257 = make_float3 (_S265 * _S265);
            _S258 = _S266;
        }
        else
        {
            _S257 = _S244;
            _S258 = _S244;
        }
        float3  _S267 = _S258;
        _S253 = _S264;
        _S258 = _S263;
        _S259 = _S267;
        _S260 = dx_0;
        _S261 = _S262;
    }
    else
    {
        _S253 = false;
        _S257 = _S244;
        _S258 = _S244;
        _S259 = _S244;
        _S260 = _S244;
        _S261 = _S244;
    }
    FixedArray<float3 , 4>  _S268;
    if(_S256)
    {
        if(_S253)
        {
            float3  _S269 = _s_dOut_8 / _S257;
            float3  _S270 = _S258 * - _S269;
            float3  _S271 = _S259 * _S269;
            float _S272 = _S270.x + _S270.y + _S270.z;
            DiffPair_vectorx3Cfloatx2C3x3E_0 _S273;
            (&_S273)->primal_0 = _S258;
            (&_S273)->differential_0 = _S244;
            s_bwd_length_impl_0(&_S273, _S272);
            _S257 = _S271 + _S273.differential_0;
        }
        else
        {
            _S257 = _s_dOut_8;
        }
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S274;
        (&_S274)->primal_0 = _S258;
        (&_S274)->differential_0 = _S244;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S275;
        (&_S275)->primal_0 = _S258;
        (&_S275)->differential_0 = _S244;
        s_bwd_prop_dot_0(&_S274, &_S275, 0.0f);
        float3  _S276 = _S275.differential_0 + _S274.differential_0 + _S257;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S277;
        (&_S277)->primal_0 = _S260;
        (&_S277)->differential_0 = _S244;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S278;
        (&_S278)->primal_0 = _S261;
        (&_S278)->differential_0 = _S244;
        s_bwd_prop_cross_0(&_S277, &_S278, _S276);
        float3  s_diff_dy_T_0 = - _S278.differential_0;
        float3  _S279 = - s_diff_dy_T_0;
        float3  _S280 = - _S277.differential_0;
        FixedArray<float3 , 4>  _S281;
        _S281[int(0)] = _S244;
        _S281[int(1)] = _S244;
        _S281[int(2)] = _S244;
        _S281[int(3)] = _S244;
        _S281[int(2)] = _S279;
        _S281[int(3)] = s_diff_dy_T_0;
        _S281[int(1)] = _S277.differential_0;
        _S268[int(0)] = _S281[int(0)];
        _S268[int(1)] = _S281[int(1)];
        _S268[int(2)] = _S281[int(2)];
        _S268[int(3)] = _S281[int(3)];
        _S257 = _S280;
    }
    else
    {
        _S268[int(0)] = _S244;
        _S268[int(1)] = _S244;
        _S268[int(2)] = _S244;
        _S268[int(3)] = _S244;
        _S257 = _S244;
    }
    if(_S250)
    {
    }
    else
    {
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S282;
        (&_S282)->primal_0 = _S254;
        (&_S282)->differential_0 = _S244;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S283;
        (&_S283)->primal_0 = _S254;
        (&_S283)->differential_0 = _S244;
        s_bwd_prop_dot_0(&_S282, &_S283, 0.0f);
        float3  _S284 = _S283.differential_0 + _S282.differential_0;
        FixedArray<float3 , 4>  _S285;
        _S285[int(0)] = _S244;
        _S285[int(1)] = _S244;
        _S285[int(2)] = _S244;
        _S285[int(3)] = _S244;
        _S285[int(3)] = _S284;
        float3  _S286 = _S268[int(1)] + _S285[int(1)];
        float3  _S287 = _S268[int(2)] + _S285[int(2)];
        float3  _S288 = _S268[int(3)] + _S285[int(3)];
        _S268[int(0)] = _S268[int(0)] + _S285[int(0)];
        _S268[int(1)] = _S286;
        _S268[int(2)] = _S287;
        _S268[int(3)] = _S288;
    }
    if(_S247)
    {
    }
    else
    {
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S289;
        (&_S289)->primal_0 = _S251;
        (&_S289)->differential_0 = _S244;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S290;
        (&_S290)->primal_0 = _S251;
        (&_S290)->differential_0 = _S244;
        s_bwd_prop_dot_0(&_S289, &_S290, 0.0f);
        float3  _S291 = _S290.differential_0 + _S289.differential_0;
        FixedArray<float3 , 4>  _S292;
        _S292[int(0)] = _S244;
        _S292[int(1)] = _S244;
        _S292[int(2)] = _S244;
        _S292[int(3)] = _S244;
        _S292[int(2)] = _S291;
        float3  _S293 = _S268[int(1)] + _S292[int(1)];
        float3  _S294 = _S268[int(2)] + _S292[int(2)];
        float3  _S295 = _S268[int(3)] + _S292[int(3)];
        _S268[int(0)] = _S268[int(0)] + _S292[int(0)];
        _S268[int(1)] = _S293;
        _S268[int(2)] = _S294;
        _S268[int(3)] = _S295;
    }
    if(_S246)
    {
    }
    else
    {
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S296;
        (&_S296)->primal_0 = _S248;
        (&_S296)->differential_0 = _S244;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S297;
        (&_S297)->primal_0 = _S248;
        (&_S297)->differential_0 = _S244;
        s_bwd_prop_dot_0(&_S296, &_S297, 0.0f);
        float3  _S298 = _S297.differential_0 + _S296.differential_0;
        FixedArray<float3 , 4>  _S299;
        _S299[int(0)] = _S244;
        _S299[int(1)] = _S244;
        _S299[int(2)] = _S244;
        _S299[int(3)] = _S244;
        _S299[int(1)] = _S298;
        float3  _S300 = _S268[int(1)] + _S299[int(1)];
        float3  _S301 = _S268[int(2)] + _S299[int(2)];
        float3  _S302 = _S268[int(3)] + _S299[int(3)];
        _S268[int(0)] = _S268[int(0)] + _S299[int(0)];
        _S268[int(1)] = _S300;
        _S268[int(2)] = _S301;
        _S268[int(3)] = _S302;
    }
    DiffPair_vectorx3Cfloatx2C3x3E_0 _S303;
    (&_S303)->primal_0 = _S243[int(0)];
    (&_S303)->differential_0 = _S244;
    DiffPair_vectorx3Cfloatx2C3x3E_0 _S304;
    (&_S304)->primal_0 = _S243[int(0)];
    (&_S304)->differential_0 = _S244;
    s_bwd_prop_dot_0(&_S303, &_S304, 0.0f);
    float3  _S305 = _S304.differential_0 + _S303.differential_0 + _S257;
    FixedArray<float3 , 4>  _S306;
    _S306[int(0)] = _S244;
    _S306[int(1)] = _S244;
    _S306[int(2)] = _S244;
    _S306[int(3)] = _S244;
    _S306[int(0)] = _S305;
    FixedArray<float3 , 4>  _S307 = {
        _S268[int(0)] + _S306[int(0)], _S268[int(1)] + _S306[int(1)], _S268[int(2)] + _S306[int(2)], _S268[int(3)] + _S306[int(3)]
    };
    dppoints_0->primal_0 = dppoints_0->primal_0;
    dppoints_0->differential_0 = _S307;
    return;
}

inline __device__ void s_bwd_points_to_normal_0(DiffPair_arrayx3Cvectorx3Cfloatx2C3x3Ex2C4x3E_0 * _S308, float3  _S309)
{
    s_bwd_prop_points_to_normal_0(_S308, _S309);
    return;
}

inline __device__ void points_to_normal_vjp(FixedArray<float3 , 4>  points_1, float3  v_normal_0, FixedArray<float3 , 4>  * v_points_0)
{
    FixedArray<float3 , 4>  _S310 = { make_float3 (0.0f), make_float3 (0.0f), make_float3 (0.0f), make_float3 (0.0f) };
    DiffPair_arrayx3Cvectorx3Cfloatx2C3x3Ex2C4x3E_0 dp_points_0;
    (&dp_points_0)->primal_0 = points_1;
    (&dp_points_0)->differential_0 = _S310;
    s_bwd_points_to_normal_0(&dp_points_0, v_normal_0);
    *v_points_0 = (&dp_points_0)->differential_0;
    return;
}

inline __device__ Matrix<float, 2, 2>  transpose_0(Matrix<float, 2, 2>  x_19)
{
    Matrix<float, 2, 2>  result_7;
    int r_1 = int(0);
    for(;;)
    {
        if(r_1 < int(2))
        {
        }
        else
        {
            break;
        }
        int c_1 = int(0);
        for(;;)
        {
            if(c_1 < int(2))
            {
            }
            else
            {
                break;
            }
            *_slang_vector_get_element_ptr(((&result_7)->rows + (r_1)), c_1) = _slang_vector_get_element(x_19.rows[c_1], r_1);
            c_1 = c_1 + int(1);
        }
        r_1 = r_1 + int(1);
    }
    return result_7;
}

inline __device__ float determinant_0(Matrix<float, 2, 2>  m_0)
{
    return m_0.rows[int(0)].x * m_0.rows[int(1)].y - m_0.rows[int(0)].y * m_0.rows[int(1)].x;
}

inline __device__ bool undistort_point_0(float2  uv_0, FixedArray<float, 1>  * dist_coeffs_0, int maxiter_0, float2  * uv_undist_0)
{
    *uv_undist_0 = uv_0;
    return true;
}

inline __device__ float2  DistOpenCV_distort_0(float2  uv_1, FixedArray<float, 4>  * coeffs_0)
{
    float u_0 = uv_1.x;
    float v_0 = uv_1.y;
    float r2_0 = u_0 * u_0 + v_0 * v_0;
    return uv_1 * make_float2 (1.0f + r2_0 * ((*coeffs_0)[int(0)] + r2_0 * (*coeffs_0)[int(1)])) + make_float2 (2.0f * (*coeffs_0)[int(2)] * u_0 * v_0 + (*coeffs_0)[int(3)] * (r2_0 + 2.0f * u_0 * u_0), 2.0f * (*coeffs_0)[int(3)] * u_0 * v_0 + (*coeffs_0)[int(2)] * (r2_0 + 2.0f * v_0 * v_0));
}

struct DiffPair_vectorx3Cfloatx2C2x3E_0
{
    float2  primal_0;
    float2  differential_0;
};

inline __device__ DiffPair_vectorx3Cfloatx2C2x3E_0 s_fwd_DistOpenCV_distort_0(DiffPair_vectorx3Cfloatx2C2x3E_0 * dpuv_0, FixedArray<float, 4>  * coeffs_1)
{
    float u_1 = dpuv_0->primal_0.x;
    float s_diff_u_0 = dpuv_0->differential_0.x;
    float v_1 = dpuv_0->primal_0.y;
    float s_diff_v_0 = dpuv_0->differential_0.y;
    float _S311 = s_diff_u_0 * u_1;
    float _S312 = s_diff_v_0 * v_1;
    float r2_1 = u_1 * u_1 + v_1 * v_1;
    float s_diff_r2_0 = _S311 + _S311 + (_S312 + _S312);
    float _S313 = (*coeffs_1)[int(0)] + r2_1 * (*coeffs_1)[int(1)];
    float radial_0 = 1.0f + r2_1 * _S313;
    float _S314 = 2.0f * (*coeffs_1)[int(2)];
    float _S315 = _S314 * u_1;
    float _S316 = 2.0f * u_1;
    float _S317 = 2.0f * (*coeffs_1)[int(3)];
    float _S318 = _S317 * u_1;
    float _S319 = 2.0f * v_1;
    DiffPair_vectorx3Cfloatx2C2x3E_0 _S320 = { dpuv_0->primal_0 * make_float2 (radial_0) + make_float2 (_S315 * v_1 + (*coeffs_1)[int(3)] * (r2_1 + _S316 * u_1), _S318 * v_1 + (*coeffs_1)[int(2)] * (r2_1 + _S319 * v_1)), dpuv_0->differential_0 * make_float2 (radial_0) + make_float2 (s_diff_r2_0 * _S313 + s_diff_r2_0 * (*coeffs_1)[int(1)] * r2_1) * dpuv_0->primal_0 + make_float2 (s_diff_u_0 * _S314 * v_1 + s_diff_v_0 * _S315 + (s_diff_r2_0 + (s_diff_u_0 * 2.0f * u_1 + s_diff_u_0 * _S316)) * (*coeffs_1)[int(3)], s_diff_u_0 * _S317 * v_1 + s_diff_v_0 * _S318 + (s_diff_r2_0 + (s_diff_v_0 * 2.0f * v_1 + s_diff_v_0 * _S319)) * (*coeffs_1)[int(2)]) };
    return _S320;
}

inline __device__ bool undistort_point_1(float2  uv_2, FixedArray<float, 4>  * dist_coeffs_1, int maxiter_1, float2  * uv_undist_1)
{
    int i_5 = int(0);
    float2  q_0 = uv_2;
    for(;;)
    {
        if(i_5 < maxiter_1)
        {
        }
        else
        {
            break;
        }
        float2  _S321 = DistOpenCV_distort_0(q_0, dist_coeffs_1);
        float2  r_2 = _S321 - uv_2;
        float2  _S322 = make_float2 (1.0f, 0.0f);
        DiffPair_vectorx3Cfloatx2C2x3E_0 _S323;
        (&_S323)->primal_0 = q_0;
        (&_S323)->differential_0 = _S322;
        DiffPair_vectorx3Cfloatx2C2x3E_0 _S324 = s_fwd_DistOpenCV_distort_0(&_S323, dist_coeffs_1);
        float2  _S325 = make_float2 (0.0f, 1.0f);
        DiffPair_vectorx3Cfloatx2C2x3E_0 _S326;
        (&_S326)->primal_0 = q_0;
        (&_S326)->differential_0 = _S325;
        DiffPair_vectorx3Cfloatx2C2x3E_0 _S327 = s_fwd_DistOpenCV_distort_0(&_S326, dist_coeffs_1);
        Matrix<float, 2, 2>  _S328 = transpose_0(makeMatrix<float, 2, 2> (_S324.differential_0, _S327.differential_0));
        float inv_det_0 = 1.0f / (_S328.rows[int(0)].x * _S328.rows[int(1)].y - _S328.rows[int(0)].y * _S328.rows[int(1)].x);
        float _S329 = r_2.x;
        float _S330 = r_2.y;
        float2  q_1 = q_0 - make_float2 ((_S329 * _S328.rows[int(1)].y - _S330 * _S328.rows[int(0)].y) * inv_det_0, (- _S329 * _S328.rows[int(1)].x + _S330 * _S328.rows[int(0)].x) * inv_det_0);
        i_5 = i_5 + int(1);
        q_0 = q_1;
    }
    *uv_undist_1 = q_0;
    float2  _S331 = make_float2 (1.0f, 0.0f);
    DiffPair_vectorx3Cfloatx2C2x3E_0 _S332;
    (&_S332)->primal_0 = q_0;
    (&_S332)->differential_0 = _S331;
    DiffPair_vectorx3Cfloatx2C2x3E_0 _S333 = s_fwd_DistOpenCV_distort_0(&_S332, dist_coeffs_1);
    float2  _S334 = make_float2 (0.0f, 1.0f);
    DiffPair_vectorx3Cfloatx2C2x3E_0 _S335;
    (&_S335)->primal_0 = q_0;
    (&_S335)->differential_0 = _S334;
    DiffPair_vectorx3Cfloatx2C2x3E_0 _S336 = s_fwd_DistOpenCV_distort_0(&_S335, dist_coeffs_1);
    Matrix<float, 2, 2>  _S337 = transpose_0(makeMatrix<float, 2, 2> (_S333.differential_0, _S336.differential_0));
    float _S338 = (F32_min((determinant_0(_S337)), ((F32_min((_S337.rows[int(0)].x), (_S337.rows[int(1)].y))))));
    bool _S339;
    if(_S338 > 0.25f)
    {
        _S339 = _S338 < 4.0f;
    }
    else
    {
        _S339 = false;
    }
    if(_S339)
    {
        float2  _S340 = DistOpenCV_distort_0(q_0, dist_coeffs_1);
        _S339 = (dot_1(q_0, _S340)) >= 0.0f;
    }
    else
    {
        _S339 = false;
    }
    if(_S339)
    {
        float2  _S341 = DistOpenCV_distort_0(*uv_undist_1, dist_coeffs_1);
        _S339 = (length_1(_S341 - uv_2)) < 0.00999999977648258f;
    }
    else
    {
        _S339 = false;
    }
    return _S339;
}

inline __device__ float2  DistThinPrism_distort_0(float2  uv_3, FixedArray<float, 8>  * coeffs_2)
{
    float u_2 = uv_3.x;
    float v_2 = uv_3.y;
    float r2_2 = u_2 * u_2 + v_2 * v_2;
    return uv_3 * make_float2 (1.0f + r2_2 * ((*coeffs_2)[int(0)] + r2_2 * ((*coeffs_2)[int(1)] + r2_2 * ((*coeffs_2)[int(2)] + r2_2 * (*coeffs_2)[int(3)])))) + make_float2 (2.0f * (*coeffs_2)[int(4)] * u_2 * v_2 + (*coeffs_2)[int(5)] * (r2_2 + 2.0f * u_2 * u_2) + (*coeffs_2)[int(6)] * r2_2, 2.0f * (*coeffs_2)[int(5)] * u_2 * v_2 + (*coeffs_2)[int(4)] * (r2_2 + 2.0f * v_2 * v_2) + (*coeffs_2)[int(7)] * r2_2);
}

inline __device__ DiffPair_vectorx3Cfloatx2C2x3E_0 s_fwd_DistThinPrism_distort_0(DiffPair_vectorx3Cfloatx2C2x3E_0 * dpuv_1, FixedArray<float, 8>  * coeffs_3)
{
    float u_3 = dpuv_1->primal_0.x;
    float s_diff_u_1 = dpuv_1->differential_0.x;
    float v_3 = dpuv_1->primal_0.y;
    float s_diff_v_1 = dpuv_1->differential_0.y;
    float _S342 = s_diff_u_1 * u_3;
    float _S343 = s_diff_v_1 * v_3;
    float r2_3 = u_3 * u_3 + v_3 * v_3;
    float s_diff_r2_1 = _S342 + _S342 + (_S343 + _S343);
    float _S344 = (*coeffs_3)[int(2)] + r2_3 * (*coeffs_3)[int(3)];
    float _S345 = (*coeffs_3)[int(1)] + r2_3 * _S344;
    float _S346 = (*coeffs_3)[int(0)] + r2_3 * _S345;
    float radial_1 = 1.0f + r2_3 * _S346;
    float _S347 = 2.0f * (*coeffs_3)[int(4)];
    float _S348 = _S347 * u_3;
    float _S349 = 2.0f * u_3;
    float _S350 = 2.0f * (*coeffs_3)[int(5)];
    float _S351 = _S350 * u_3;
    float _S352 = 2.0f * v_3;
    DiffPair_vectorx3Cfloatx2C2x3E_0 _S353 = { dpuv_1->primal_0 * make_float2 (radial_1) + make_float2 (_S348 * v_3 + (*coeffs_3)[int(5)] * (r2_3 + _S349 * u_3) + (*coeffs_3)[int(6)] * r2_3, _S351 * v_3 + (*coeffs_3)[int(4)] * (r2_3 + _S352 * v_3) + (*coeffs_3)[int(7)] * r2_3), dpuv_1->differential_0 * make_float2 (radial_1) + make_float2 (s_diff_r2_1 * _S346 + (s_diff_r2_1 * _S345 + (s_diff_r2_1 * _S344 + s_diff_r2_1 * (*coeffs_3)[int(3)] * r2_3) * r2_3) * r2_3) * dpuv_1->primal_0 + make_float2 (s_diff_u_1 * _S347 * v_3 + s_diff_v_1 * _S348 + (s_diff_r2_1 + (s_diff_u_1 * 2.0f * u_3 + s_diff_u_1 * _S349)) * (*coeffs_3)[int(5)] + s_diff_r2_1 * (*coeffs_3)[int(6)], s_diff_u_1 * _S350 * v_3 + s_diff_v_1 * _S351 + (s_diff_r2_1 + (s_diff_v_1 * 2.0f * v_3 + s_diff_v_1 * _S352)) * (*coeffs_3)[int(4)] + s_diff_r2_1 * (*coeffs_3)[int(7)]) };
    return _S353;
}

inline __device__ bool undistort_point_2(float2  uv_4, FixedArray<float, 8>  * dist_coeffs_2, int maxiter_2, float2  * uv_undist_2)
{
    int i_6 = int(0);
    float2  q_2 = uv_4;
    for(;;)
    {
        if(i_6 < maxiter_2)
        {
        }
        else
        {
            break;
        }
        float2  _S354 = DistThinPrism_distort_0(q_2, dist_coeffs_2);
        float2  r_3 = _S354 - uv_4;
        float2  _S355 = make_float2 (1.0f, 0.0f);
        DiffPair_vectorx3Cfloatx2C2x3E_0 _S356;
        (&_S356)->primal_0 = q_2;
        (&_S356)->differential_0 = _S355;
        DiffPair_vectorx3Cfloatx2C2x3E_0 _S357 = s_fwd_DistThinPrism_distort_0(&_S356, dist_coeffs_2);
        float2  _S358 = make_float2 (0.0f, 1.0f);
        DiffPair_vectorx3Cfloatx2C2x3E_0 _S359;
        (&_S359)->primal_0 = q_2;
        (&_S359)->differential_0 = _S358;
        DiffPair_vectorx3Cfloatx2C2x3E_0 _S360 = s_fwd_DistThinPrism_distort_0(&_S359, dist_coeffs_2);
        Matrix<float, 2, 2>  _S361 = transpose_0(makeMatrix<float, 2, 2> (_S357.differential_0, _S360.differential_0));
        float inv_det_1 = 1.0f / (_S361.rows[int(0)].x * _S361.rows[int(1)].y - _S361.rows[int(0)].y * _S361.rows[int(1)].x);
        float _S362 = r_3.x;
        float _S363 = r_3.y;
        float2  q_3 = q_2 - make_float2 ((_S362 * _S361.rows[int(1)].y - _S363 * _S361.rows[int(0)].y) * inv_det_1, (- _S362 * _S361.rows[int(1)].x + _S363 * _S361.rows[int(0)].x) * inv_det_1);
        i_6 = i_6 + int(1);
        q_2 = q_3;
    }
    *uv_undist_2 = q_2;
    float2  _S364 = make_float2 (1.0f, 0.0f);
    DiffPair_vectorx3Cfloatx2C2x3E_0 _S365;
    (&_S365)->primal_0 = q_2;
    (&_S365)->differential_0 = _S364;
    DiffPair_vectorx3Cfloatx2C2x3E_0 _S366 = s_fwd_DistThinPrism_distort_0(&_S365, dist_coeffs_2);
    float2  _S367 = make_float2 (0.0f, 1.0f);
    DiffPair_vectorx3Cfloatx2C2x3E_0 _S368;
    (&_S368)->primal_0 = q_2;
    (&_S368)->differential_0 = _S367;
    DiffPair_vectorx3Cfloatx2C2x3E_0 _S369 = s_fwd_DistThinPrism_distort_0(&_S368, dist_coeffs_2);
    Matrix<float, 2, 2>  _S370 = transpose_0(makeMatrix<float, 2, 2> (_S366.differential_0, _S369.differential_0));
    float _S371 = (F32_min((determinant_0(_S370)), ((F32_min((_S370.rows[int(0)].x), (_S370.rows[int(1)].y))))));
    bool _S372;
    if(_S371 > 0.25f)
    {
        _S372 = _S371 < 4.0f;
    }
    else
    {
        _S372 = false;
    }
    if(_S372)
    {
        float2  _S373 = DistThinPrism_distort_0(q_2, dist_coeffs_2);
        _S372 = (dot_1(q_2, _S373)) >= 0.0f;
    }
    else
    {
        _S372 = false;
    }
    if(_S372)
    {
        float2  _S374 = DistThinPrism_distort_0(*uv_undist_2, dist_coeffs_2);
        _S372 = (length_1(_S374 - uv_4)) < 0.00999999977648258f;
    }
    else
    {
        _S372 = false;
    }
    return _S372;
}

inline __device__ float3  normalize_0(float3  x_20)
{
    return x_20 / make_float3 (length_0(x_20));
}

inline __device__ float3  unproject_raydir_0(float2  uv_5, int camera_model_0, bool is_ray_depth_0)
{
    float3  raydir_0;
    bool is_unit_0;
    if(camera_model_0 == int(1))
    {
        float theta_0 = length_1(uv_5);
        float3  _S375 = make_float3 ((uv_5 / make_float2 ((F32_max((theta_0), (1.00000001168609742e-07f)))) * make_float2 ((F32_sin((theta_0))))).x, (uv_5 / make_float2 ((F32_max((theta_0), (1.00000001168609742e-07f)))) * make_float2 ((F32_sin((theta_0))))).y, (F32_cos((theta_0))));
        is_unit_0 = true;
        raydir_0 = _S375;
    }
    else
    {
        bool _S376 = camera_model_0 == int(2);
        if(_S376)
        {
            float r_4 = length_1(uv_5);
            raydir_0 = make_float3 ((uv_5 * make_float2 ((F32_sqrt(((F32_max((0.0f), (1.0f - 0.25f * r_4 * r_4)))))))).x, (uv_5 * make_float2 ((F32_sqrt(((F32_max((0.0f), (1.0f - 0.25f * r_4 * r_4)))))))).y, 1.0f - 0.5f * r_4 * r_4);
        }
        else
        {
            raydir_0 = make_float3 (uv_5.x, uv_5.y, 1.0f);
        }
        is_unit_0 = _S376;
    }
    if(is_ray_depth_0)
    {
        if(is_unit_0)
        {
        }
        else
        {
            raydir_0 = normalize_0(raydir_0);
        }
    }
    else
    {
        raydir_0 = raydir_0 / make_float3 (raydir_0.z);
    }
    return raydir_0;
}

inline __device__ float3  generate_ray_d2n_none(float2  pix_pos_0, float4  intrins_0, FixedArray<float, 1>  dist_coeffs_3, int camera_model_1, bool is_ray_depth_1)
{
    float3  _S377;
    for(;;)
    {
        float2  uv_6 = (pix_pos_0 - float2 {intrins_0.z, intrins_0.w}) / float2 {intrins_0.x, intrins_0.y};
        FixedArray<float, 1>  _S378 = dist_coeffs_3;
        float2  uv_u_0;
        bool _S379 = undistort_point_0(uv_6, &_S378, int(12), &uv_u_0);
        if(!_S379)
        {
            int3  _S380 = make_int3 (int(0));
            float3  _S381 = make_float3 ((float)_S380.x, (float)_S380.y, (float)_S380.z);
            _S377 = _S381;
            break;
        }
        _S377 = unproject_raydir_0(uv_u_0, camera_model_1, is_ray_depth_1);
        break;
    }
    return _S377;
}

inline __device__ float3  depth_to_point_none(float2  pix_pos_1, float4  intrins_1, FixedArray<float, 1>  dist_coeffs_4, int camera_model_2, bool is_ray_depth_2, float depth_2)
{
    float3  _S382;
    for(;;)
    {
        float2  uv_7 = (pix_pos_1 - float2 {intrins_1.z, intrins_1.w}) / float2 {intrins_1.x, intrins_1.y};
        FixedArray<float, 1>  _S383 = dist_coeffs_4;
        float2  uv_u_1;
        bool _S384 = undistort_point_0(uv_7, &_S383, int(12), &uv_u_1);
        if(!_S384)
        {
            _S382 = make_float3 (0.0f);
            break;
        }
        _S382 = make_float3 (depth_2) * unproject_raydir_0(uv_u_1, camera_model_2, is_ray_depth_2);
        break;
    }
    return _S382;
}

struct s_bwd_prop_depth_to_point_Intermediates_0
{
    float2  _S385;
    bool _S386;
};

inline __device__ float s_primal_ctx_sin_0(float _S387)
{
    return (F32_sin((_S387)));
}

inline __device__ float s_primal_ctx_cos_0(float _S388)
{
    return (F32_cos((_S388)));
}

inline __device__ float s_primal_ctx_sqrt_0(float _S389)
{
    return (F32_sqrt((_S389)));
}

inline __device__ float3  s_primal_ctx_unproject_raydir_0(float2  dpuv_2, int camera_model_3, bool is_ray_depth_3)
{
    float3  raydir_1;
    bool is_unit_1;
    if(camera_model_3 == int(1))
    {
        float _S390 = length_1(dpuv_2);
        float3  _S391 = make_float3 ((dpuv_2 / make_float2 ((F32_max((_S390), (1.00000001168609742e-07f)))) * make_float2 (s_primal_ctx_sin_0(_S390))).x, (dpuv_2 / make_float2 ((F32_max((_S390), (1.00000001168609742e-07f)))) * make_float2 (s_primal_ctx_sin_0(_S390))).y, s_primal_ctx_cos_0(_S390));
        is_unit_1 = true;
        raydir_1 = _S391;
    }
    else
    {
        bool _S392 = camera_model_3 == int(2);
        if(_S392)
        {
            float _S393 = length_1(dpuv_2);
            raydir_1 = make_float3 ((dpuv_2 * make_float2 (s_primal_ctx_sqrt_0((F32_max((0.0f), (1.0f - 0.25f * _S393 * _S393)))))).x, (dpuv_2 * make_float2 (s_primal_ctx_sqrt_0((F32_max((0.0f), (1.0f - 0.25f * _S393 * _S393)))))).y, 1.0f - 0.5f * _S393 * _S393);
        }
        else
        {
            raydir_1 = make_float3 (dpuv_2.x, dpuv_2.y, 1.0f);
        }
        is_unit_1 = _S392;
    }
    if(is_ray_depth_3)
    {
        if(is_unit_1)
        {
        }
        else
        {
            raydir_1 = normalize_0(raydir_1);
        }
    }
    else
    {
        raydir_1 = raydir_1 / make_float3 (raydir_1.z);
    }
    return raydir_1;
}

inline __device__ float depth_to_point_vjp_none(float2  pix_pos_2, float4  intrins_2, FixedArray<float, 1>  dist_coeffs_5, int camera_model_4, bool is_ray_depth_4, float depth_3, float3  v_point_0)
{
    float2  _S394 = make_float2 (0.0f);
    s_bwd_prop_depth_to_point_Intermediates_0 _S395;
    (&_S395)->_S385 = _S394;
    (&_S395)->_S386 = false;
    float2  uv_8 = (pix_pos_2 - float2 {intrins_2.z, intrins_2.w}) / float2 {intrins_2.x, intrins_2.y};
    float2  _S396 = _S394;
    FixedArray<float, 1>  _S397 = dist_coeffs_5;
    bool _S398 = undistort_point_0(uv_8, &_S397, int(12), &_S396);
    (&_S395)->_S385 = _S396;
    (&_S395)->_S386 = _S398;
    s_bwd_prop_depth_to_point_Intermediates_0 _S399 = _S395;
    float3  _S400 = make_float3 (0.0f);
    bool _S401 = !!_S395._S386;
    float3  _S402;
    if(_S401)
    {
        _S402 = s_primal_ctx_unproject_raydir_0(_S399._S385, camera_model_4, is_ray_depth_4);
    }
    else
    {
        _S402 = _S400;
    }
    if(_S401)
    {
        _S402 = _S402 * v_point_0;
    }
    else
    {
        _S402 = _S400;
    }
    return _S402.x + _S402.y + _S402.z;
}

inline __device__ float3  depth_to_normal_none(float2  pix_center_0, float4  intrins_3, FixedArray<float, 1>  dist_coeffs_6, int camera_model_5, bool is_ray_depth_5, float4  depths_0)
{
    float3  normal_2;
    for(;;)
    {
        bool _S403;
        if((depths_0.x) == 0.0f)
        {
            _S403 = true;
        }
        else
        {
            _S403 = (depths_0.y) == 0.0f;
        }
        if(_S403)
        {
            _S403 = true;
        }
        else
        {
            _S403 = (depths_0.z) == 0.0f;
        }
        if(_S403)
        {
            _S403 = true;
        }
        else
        {
            _S403 = (depths_0.w) == 0.0f;
        }
        if(_S403)
        {
            normal_2 = make_float3 (0.0f);
            break;
        }
        float3  * _S404;
        float3  * _S405;
        float3  * _S406;
        float3  * _S407;
        int _S408;
        FixedArray<float3 , 4>  points_2;
        for(;;)
        {
            float2  _S409 = float2 {intrins_3.z, intrins_3.w};
            float2  _S410 = float2 {intrins_3.x, intrins_3.y};
            float2  uv_9 = (pix_center_0 + make_float2 (-1.0f, -0.0f) - _S409) / _S410;
            FixedArray<float, 1>  _S411 = dist_coeffs_6;
            float2  uv_u_2;
            bool _S412 = undistort_point_0(uv_9, &_S411, int(12), &uv_u_2);
            if(!_S412)
            {
                float3  _S413 = make_float3 (0.0f);
                _S408 = int(0);
                _S407 = nullptr;
                _S406 = nullptr;
                _S405 = nullptr;
                _S404 = nullptr;
                normal_2 = _S413;
                break;
            }
            points_2[int(0)] = make_float3 (depths_0.x) * unproject_raydir_0(uv_u_2, camera_model_5, is_ray_depth_5);
            for(;;)
            {
                float2  uv_10 = (pix_center_0 + make_float2 (1.0f, -0.0f) - _S409) / _S410;
                FixedArray<float, 1>  _S414 = dist_coeffs_6;
                float2  uv_u_3;
                bool _S415 = undistort_point_0(uv_10, &_S414, int(12), &uv_u_3);
                if(!_S415)
                {
                    float3  _S416 = make_float3 (0.0f);
                    _S408 = int(0);
                    _S407 = nullptr;
                    normal_2 = _S416;
                    break;
                }
                points_2[int(1)] = make_float3 (depths_0.y) * unproject_raydir_0(uv_u_3, camera_model_5, is_ray_depth_5);
                _S408 = int(2);
                _S407 = &points_2[int(1)];
                break;
            }
            if(_S408 != int(2))
            {
                _S406 = &points_2[int(0)];
                _S405 = nullptr;
                _S404 = nullptr;
                break;
            }
            float2  uv_11 = (pix_center_0 + make_float2 (0.0f, -1.0f) - _S409) / _S410;
            FixedArray<float, 1>  _S417 = dist_coeffs_6;
            float2  uv_u_4;
            bool _S418 = undistort_point_0(uv_11, &_S417, int(12), &uv_u_4);
            if(!_S418)
            {
                float3  _S419 = make_float3 (0.0f);
                _S408 = int(0);
                _S406 = &points_2[int(0)];
                _S405 = nullptr;
                _S404 = nullptr;
                normal_2 = _S419;
                break;
            }
            points_2[int(2)] = make_float3 (depths_0.z) * unproject_raydir_0(uv_u_4, camera_model_5, is_ray_depth_5);
            for(;;)
            {
                float2  uv_12 = (pix_center_0 + make_float2 (0.0f, 1.0f) - _S409) / _S410;
                FixedArray<float, 1>  _S420 = dist_coeffs_6;
                float2  uv_u_5;
                bool _S421 = undistort_point_0(uv_12, &_S420, int(12), &uv_u_5);
                if(!_S421)
                {
                    float3  _S422 = make_float3 (0.0f);
                    _S408 = int(0);
                    _S406 = nullptr;
                    normal_2 = _S422;
                    break;
                }
                points_2[int(3)] = make_float3 (depths_0.w) * unproject_raydir_0(uv_u_5, camera_model_5, is_ray_depth_5);
                _S408 = int(2);
                _S406 = &points_2[int(3)];
                break;
            }
            if(_S408 != int(2))
            {
                float3  * _S423 = _S406;
                _S406 = &points_2[int(0)];
                _S405 = _S423;
                _S404 = &points_2[int(2)];
                break;
            }
            float3  * _S424 = _S406;
            _S408 = int(1);
            _S406 = &points_2[int(0)];
            _S405 = _S424;
            _S404 = &points_2[int(2)];
            break;
        }
        if(_S408 != int(1))
        {
            break;
        }
        float3  normal_3 = cross_0(*_S407 - *_S406, - (*_S405 - *_S404));
        if((dot_0(normal_3, normal_3)) != 0.0f)
        {
            normal_2 = normal_3 / make_float3 (length_0(normal_3));
        }
        else
        {
            normal_2 = normal_3;
        }
        break;
    }
    return normal_2;
}

struct s_bwd_prop_depth_to_normal_Intermediates_0
{
    float2  _S425;
    bool _S426;
    float2  _S427;
    bool _S428;
    float2  _S429;
    bool _S430;
    float2  _S431;
    bool _S432;
};

inline __device__ void depth_to_normal_vjp_none(float2  pix_center_1, float4  intrins_4, FixedArray<float, 1>  dist_coeffs_7, int camera_model_6, bool is_ray_depth_6, float4  depths_1, float3  v_normal_1, float4  * v_depths_0)
{
    float2  _S433 = make_float2 (0.0f);
    s_bwd_prop_depth_to_normal_Intermediates_0 _S434;
    (&_S434)->_S425 = _S433;
    (&_S434)->_S426 = false;
    (&_S434)->_S427 = _S433;
    (&_S434)->_S428 = false;
    (&_S434)->_S429 = _S433;
    (&_S434)->_S430 = false;
    (&_S434)->_S431 = _S433;
    (&_S434)->_S432 = false;
    (&_S434)->_S425 = _S433;
    (&_S434)->_S426 = false;
    (&_S434)->_S427 = _S433;
    (&_S434)->_S428 = false;
    (&_S434)->_S429 = _S433;
    (&_S434)->_S430 = false;
    (&_S434)->_S431 = _S433;
    (&_S434)->_S432 = false;
    bool _S435 = (depths_1.x) == 0.0f;
    bool _runFlag_3;
    if(_S435)
    {
        _runFlag_3 = true;
    }
    else
    {
        _runFlag_3 = (depths_1.y) == 0.0f;
    }
    if(_runFlag_3)
    {
        _runFlag_3 = true;
    }
    else
    {
        _runFlag_3 = (depths_1.z) == 0.0f;
    }
    if(_runFlag_3)
    {
        _runFlag_3 = true;
    }
    else
    {
        _runFlag_3 = (depths_1.w) == 0.0f;
    }
    int _S436;
    if(!_runFlag_3)
    {
        float2  _S437 = float2 {intrins_4.z, intrins_4.w};
        float2  _S438 = float2 {intrins_4.x, intrins_4.y};
        float2  uv_13 = (pix_center_1 + make_float2 (-1.0f, -0.0f) - _S437) / _S438;
        float2  _S439 = _S433;
        FixedArray<float, 1>  _S440 = dist_coeffs_7;
        bool _S441 = undistort_point_0(uv_13, &_S440, int(12), &_S439);
        (&_S434)->_S425 = _S439;
        (&_S434)->_S426 = _S441;
        bool _S442 = !!_S441;
        if(_S442)
        {
            float2  uv_14 = (pix_center_1 + make_float2 (1.0f, -0.0f) - _S437) / _S438;
            float2  _S443 = _S433;
            FixedArray<float, 1>  _S444 = dist_coeffs_7;
            bool _S445 = undistort_point_0(uv_14, &_S444, int(12), &_S443);
            (&_S434)->_S427 = _S443;
            (&_S434)->_S428 = _S445;
            if(!!_S445)
            {
                _S436 = int(2);
            }
            else
            {
                _S436 = int(0);
            }
            if(_S436 != int(2))
            {
                _runFlag_3 = false;
            }
            else
            {
                _runFlag_3 = _S442;
            }
            if(_runFlag_3)
            {
                float2  uv_15 = (pix_center_1 + make_float2 (0.0f, -1.0f) - _S437) / _S438;
                float2  _S446 = _S433;
                FixedArray<float, 1>  _S447 = dist_coeffs_7;
                bool _S448 = undistort_point_0(uv_15, &_S447, int(12), &_S446);
                (&_S434)->_S429 = _S446;
                (&_S434)->_S430 = _S448;
                if(!_S448)
                {
                    _runFlag_3 = false;
                }
                if(_runFlag_3)
                {
                    float2  uv_16 = (pix_center_1 + make_float2 (0.0f, 1.0f) - _S437) / _S438;
                    float2  _S449 = _S433;
                    FixedArray<float, 1>  _S450 = dist_coeffs_7;
                    bool _S451 = undistort_point_0(uv_16, &_S450, int(12), &_S449);
                    (&_S434)->_S431 = _S449;
                    (&_S434)->_S432 = _S451;
                }
            }
        }
    }
    s_bwd_prop_depth_to_normal_Intermediates_0 _S452 = _S434;
    float3  _S453 = make_float3 (0.0f);
    if(_S435)
    {
        _runFlag_3 = true;
    }
    else
    {
        _runFlag_3 = (depths_1.y) == 0.0f;
    }
    if(_runFlag_3)
    {
        _runFlag_3 = true;
    }
    else
    {
        _runFlag_3 = (depths_1.z) == 0.0f;
    }
    if(_runFlag_3)
    {
        _runFlag_3 = true;
    }
    else
    {
        _runFlag_3 = (depths_1.w) == 0.0f;
    }
    bool _S454 = !_runFlag_3;
    bool _runFlag_4;
    bool _runFlag_5;
    bool _S455;
    bool _runFlag_6;
    bool _S456;
    bool _S457;
    FixedArray<float3 , 4>  points_3;
    float3  _S458;
    float3  _S459;
    float3  _S460;
    float3  _S461;
    float3  _S462;
    float3  _S463;
    float3  _S464;
    float3  _S465;
    float3  _S466;
    if(_S454)
    {
        bool _S467 = !!_S452._S426;
        if(_S467)
        {
            float3  _S468 = s_primal_ctx_unproject_raydir_0(_S452._S425, camera_model_6, is_ray_depth_6);
            float3  _S469 = make_float3 (depths_1.x) * _S468;
            bool _S470 = !!_S452._S428;
            if(_S470)
            {
                float3  _S471 = s_primal_ctx_unproject_raydir_0(_S452._S427, camera_model_6, is_ray_depth_6);
                float3  _S472 = make_float3 (depths_1.y) * _S471;
                _S436 = int(2);
                points_3[int(0)] = _S469;
                points_3[int(1)] = _S472;
                points_3[int(2)] = _S453;
                points_3[int(3)] = _S453;
                _S458 = _S471;
            }
            else
            {
                _S436 = int(0);
                points_3[int(0)] = _S469;
                points_3[int(1)] = _S453;
                points_3[int(2)] = _S453;
                points_3[int(3)] = _S453;
                _S458 = _S453;
            }
            if(_S436 != int(2))
            {
                _runFlag_3 = false;
            }
            else
            {
                _runFlag_3 = _S467;
                _S436 = int(0);
            }
            if(_runFlag_3)
            {
                if(!_S452._S430)
                {
                    _runFlag_4 = false;
                    _S436 = int(0);
                }
                else
                {
                    _runFlag_4 = _runFlag_3;
                }
                if(_runFlag_4)
                {
                    float3  _S473 = s_primal_ctx_unproject_raydir_0(_S452._S429, camera_model_6, is_ray_depth_6);
                    points_3[int(2)] = make_float3 (depths_1.z) * _S473;
                    bool _S474 = !!_S452._S432;
                    int _S475;
                    if(_S474)
                    {
                        float3  _S476 = s_primal_ctx_unproject_raydir_0(_S452._S431, camera_model_6, is_ray_depth_6);
                        points_3[int(3)] = make_float3 (depths_1.w) * _S476;
                        _S475 = int(2);
                        _S459 = _S476;
                    }
                    else
                    {
                        _S475 = int(0);
                        _S459 = _S453;
                    }
                    if(_S475 != int(2))
                    {
                        _runFlag_5 = false;
                        _S436 = _S475;
                    }
                    else
                    {
                        _runFlag_5 = _runFlag_4;
                    }
                    if(_runFlag_5)
                    {
                        _S436 = int(1);
                    }
                    _runFlag_5 = _S474;
                    _S460 = _S473;
                }
                else
                {
                    _runFlag_5 = false;
                    _S459 = _S453;
                    _S460 = _S453;
                }
            }
            else
            {
                _runFlag_4 = false;
                _runFlag_5 = false;
                _S459 = _S453;
                _S460 = _S453;
            }
            float3  _S477 = _S458;
            _S458 = _S459;
            _S459 = _S460;
            _S455 = _S470;
            _S460 = _S477;
            _S461 = _S468;
        }
        else
        {
            _S436 = int(0);
            points_3[int(0)] = _S453;
            points_3[int(1)] = _S453;
            points_3[int(2)] = _S453;
            points_3[int(3)] = _S453;
            _runFlag_3 = false;
            _runFlag_4 = false;
            _runFlag_5 = false;
            _S458 = _S453;
            _S459 = _S453;
            _S455 = false;
            _S460 = _S453;
            _S461 = _S453;
        }
        if(_S436 != int(1))
        {
            _runFlag_6 = false;
        }
        else
        {
            _runFlag_6 = _S454;
        }
        if(_runFlag_6)
        {
            float3  dx_1 = points_3[int(1)] - points_3[int(0)];
            float3  _S478 = - (points_3[int(3)] - points_3[int(2)]);
            float3  _S479 = s_primal_ctx_cross_0(dx_1, _S478);
            bool _S480 = (s_primal_ctx_dot_0(_S479, _S479)) != 0.0f;
            if(_S480)
            {
                float _S481 = length_0(_S479);
                float3  _S482 = make_float3 (_S481);
                _S462 = make_float3 (_S481 * _S481);
                _S463 = _S482;
            }
            else
            {
                _S462 = _S453;
                _S463 = _S453;
            }
            float3  _S483 = _S463;
            _S456 = _S480;
            _S463 = _S479;
            _S464 = _S483;
            _S465 = dx_1;
            _S466 = _S478;
        }
        else
        {
            _S456 = false;
            _S462 = _S453;
            _S463 = _S453;
            _S464 = _S453;
            _S465 = _S453;
            _S466 = _S453;
        }
        bool _S484 = _runFlag_3;
        bool _S485 = _runFlag_4;
        bool _S486 = _runFlag_5;
        float3  _S487 = _S458;
        float3  _S488 = _S459;
        bool _S489 = _S455;
        float3  _S490 = _S460;
        float3  _S491 = _S461;
        _runFlag_3 = _runFlag_6;
        _runFlag_4 = _S456;
        _S458 = _S462;
        _S459 = _S463;
        _S460 = _S464;
        _S461 = _S465;
        _S462 = _S466;
        _runFlag_5 = _S467;
        _S455 = _S484;
        _runFlag_6 = _S485;
        _S456 = _S486;
        _S463 = _S487;
        _S464 = _S488;
        _S457 = _S489;
        _S465 = _S490;
        _S466 = _S491;
    }
    else
    {
        _runFlag_3 = false;
        _runFlag_4 = false;
        _S458 = _S453;
        _S459 = _S453;
        _S460 = _S453;
        _S461 = _S453;
        _S462 = _S453;
        _runFlag_5 = false;
        _S455 = false;
        _runFlag_6 = false;
        _S456 = false;
        _S463 = _S453;
        _S464 = _S453;
        _S457 = false;
        _S465 = _S453;
        _S466 = _S453;
    }
    float4  _S492 = make_float4 (0.0f);
    float4  _S493;
    if(_S454)
    {
        if(_runFlag_3)
        {
            if(_runFlag_4)
            {
                float3  _S494 = v_normal_1 / _S458;
                float3  _S495 = _S459 * - _S494;
                float3  _S496 = _S460 * _S494;
                float _S497 = _S495.x + _S495.y + _S495.z;
                DiffPair_vectorx3Cfloatx2C3x3E_0 _S498;
                (&_S498)->primal_0 = _S459;
                (&_S498)->differential_0 = _S453;
                s_bwd_length_impl_0(&_S498, _S497);
                _S458 = _S496 + _S498.differential_0;
            }
            else
            {
                _S458 = v_normal_1;
            }
            DiffPair_vectorx3Cfloatx2C3x3E_0 _S499;
            (&_S499)->primal_0 = _S459;
            (&_S499)->differential_0 = _S453;
            DiffPair_vectorx3Cfloatx2C3x3E_0 _S500;
            (&_S500)->primal_0 = _S459;
            (&_S500)->differential_0 = _S453;
            s_bwd_prop_dot_0(&_S499, &_S500, 0.0f);
            float3  _S501 = _S500.differential_0 + _S499.differential_0 + _S458;
            DiffPair_vectorx3Cfloatx2C3x3E_0 _S502;
            (&_S502)->primal_0 = _S461;
            (&_S502)->differential_0 = _S453;
            DiffPair_vectorx3Cfloatx2C3x3E_0 _S503;
            (&_S503)->primal_0 = _S462;
            (&_S503)->differential_0 = _S453;
            s_bwd_prop_cross_0(&_S502, &_S503, _S501);
            float3  s_diff_dy_T_1 = - _S503.differential_0;
            float3  _S504 = - s_diff_dy_T_1;
            float3  _S505 = - _S502.differential_0;
            FixedArray<float3 , 4>  _S506;
            _S506[int(0)] = _S453;
            _S506[int(1)] = _S453;
            _S506[int(2)] = _S453;
            _S506[int(3)] = _S453;
            _S506[int(2)] = _S504;
            _S506[int(3)] = s_diff_dy_T_1;
            _S506[int(0)] = _S505;
            _S506[int(1)] = _S502.differential_0;
            points_3[int(0)] = _S506[int(0)];
            points_3[int(1)] = _S506[int(1)];
            points_3[int(2)] = _S506[int(2)];
            points_3[int(3)] = _S506[int(3)];
        }
        else
        {
            points_3[int(0)] = _S453;
            points_3[int(1)] = _S453;
            points_3[int(2)] = _S453;
            points_3[int(3)] = _S453;
        }
        if(_runFlag_5)
        {
            if(_S455)
            {
                if(_runFlag_6)
                {
                    FixedArray<float3 , 4>  _S507 = points_3;
                    FixedArray<float3 , 4>  _S508 = points_3;
                    FixedArray<float3 , 4>  _S509 = points_3;
                    FixedArray<float3 , 4>  _S510 = points_3;
                    if(_S456)
                    {
                        float3  _S511 = _S463 * _S510[int(3)];
                        float _S512 = _S511.x + _S511.y + _S511.z;
                        float4  _S513 = _S492;
                        *&((&_S513)->w) = _S512;
                        points_3[int(0)] = _S507[int(0)];
                        points_3[int(1)] = _S508[int(1)];
                        points_3[int(2)] = _S509[int(2)];
                        points_3[int(3)] = _S453;
                        _S493 = _S513;
                    }
                    else
                    {
                        points_3[int(0)] = _S507[int(0)];
                        points_3[int(1)] = _S508[int(1)];
                        points_3[int(2)] = _S509[int(2)];
                        points_3[int(3)] = _S510[int(3)];
                        _S493 = _S492;
                    }
                    float3  _S514 = _S464 * points_3[int(2)];
                    float _S515 = _S514.x + _S514.y + _S514.z;
                    FixedArray<float3 , 4>  _S516 = points_3;
                    FixedArray<float3 , 4>  _S517 = points_3;
                    float4  _S518 = _S492;
                    *&((&_S518)->z) = _S515;
                    float4  _S519 = _S493 + _S518;
                    points_3[int(0)] = points_3[int(0)];
                    points_3[int(1)] = _S516[int(1)];
                    points_3[int(2)] = _S453;
                    points_3[int(3)] = _S517[int(3)];
                    _S493 = _S519;
                }
                else
                {
                    FixedArray<float3 , 4>  _S520 = points_3;
                    FixedArray<float3 , 4>  _S521 = points_3;
                    FixedArray<float3 , 4>  _S522 = points_3;
                    points_3[int(0)] = points_3[int(0)];
                    points_3[int(1)] = _S520[int(1)];
                    points_3[int(2)] = _S521[int(2)];
                    points_3[int(3)] = _S522[int(3)];
                    _S493 = _S492;
                }
            }
            else
            {
                FixedArray<float3 , 4>  _S523 = points_3;
                FixedArray<float3 , 4>  _S524 = points_3;
                FixedArray<float3 , 4>  _S525 = points_3;
                points_3[int(0)] = points_3[int(0)];
                points_3[int(1)] = _S523[int(1)];
                points_3[int(2)] = _S524[int(2)];
                points_3[int(3)] = _S525[int(3)];
                _S493 = _S492;
            }
            if(_S457)
            {
                FixedArray<float3 , 4>  _S526 = points_3;
                float3  _S527 = _S465 * points_3[int(1)];
                float _S528 = _S527.x + _S527.y + _S527.z;
                float4  _S529 = _S492;
                *&((&_S529)->y) = _S528;
                float4  _S530 = _S493 + _S529;
                points_3[int(0)] = _S453;
                points_3[int(1)] = _S453;
                points_3[int(2)] = _S453;
                points_3[int(3)] = _S453;
                _S458 = _S526[int(0)];
                _S493 = _S530;
            }
            else
            {
                FixedArray<float3 , 4>  _S531 = points_3;
                FixedArray<float3 , 4>  _S532 = points_3;
                FixedArray<float3 , 4>  _S533 = points_3;
                points_3[int(0)] = points_3[int(0)];
                points_3[int(1)] = _S531[int(1)];
                points_3[int(2)] = _S532[int(2)];
                points_3[int(3)] = _S533[int(3)];
                _S458 = _S453;
            }
            float3  _S534 = _S466 * (points_3[int(0)] + _S458);
            float _S535 = _S534.x + _S534.y + _S534.z;
            float4  _S536 = _S492;
            *&((&_S536)->x) = _S535;
            _S493 = _S493 + _S536;
        }
        else
        {
            _S493 = _S492;
        }
    }
    else
    {
        _S493 = _S492;
    }
    *v_depths_0 = _S493;
    return;
}

inline __device__ float ray_depth_to_linear_depth_factor_none(float2  pix_center_2, float4  intrins_5, FixedArray<float, 1>  dist_coeffs_8, int camera_model_7)
{
    float _S537;
    for(;;)
    {
        float2  uv_17 = (pix_center_2 - float2 {intrins_5.z, intrins_5.w}) / float2 {intrins_5.x, intrins_5.y};
        FixedArray<float, 1>  _S538 = dist_coeffs_8;
        float2  uv_u_6;
        bool _S539 = undistort_point_0(uv_17, &_S538, int(12), &uv_u_6);
        if(!_S539)
        {
            _S537 = 0.0f;
            break;
        }
        float3  raydir_2 = unproject_raydir_0(uv_u_6, camera_model_7, false);
        _S537 = float((F32_sign((raydir_2.z)))) / length_0(raydir_2);
        break;
    }
    return _S537;
}

inline __device__ float depth_normal_loss_none(float2  pix_center_3, float4  intrins_6, FixedArray<float, 1>  dist_coeffs_9, int camera_model_8, bool is_ray_depth_7, float4  depths_2, float3  gt_normal_0)
{
    float _S540;
    for(;;)
    {
        float3  _S541;
        float3  * _S542;
        float3  * _S543;
        float3  * _S544;
        float3  * _S545;
        int _S546;
        FixedArray<float3 , 5>  points_4;
        for(;;)
        {
            float2  _S547 = float2 {intrins_6.z, intrins_6.w};
            float2  _S548 = float2 {intrins_6.x, intrins_6.y};
            float2  uv_18 = (pix_center_3 + make_float2 (-1.0f, -0.0f) - _S547) / _S548;
            FixedArray<float, 1>  _S549 = dist_coeffs_9;
            float2  uv_u_7;
            bool _S550 = undistort_point_0(uv_18, &_S549, int(12), &uv_u_7);
            float3  _S551 = make_float3 (0.0f);
            if(!_S550)
            {
                _S546 = int(0);
                _S545 = nullptr;
                _S544 = nullptr;
                _S543 = nullptr;
                _S542 = nullptr;
                _S541 = _S551;
                break;
            }
            float3  raydir_3 = unproject_raydir_0(uv_u_7, camera_model_8, is_ray_depth_7);
            points_4[int(0)] = make_float3 (depths_2.x) * raydir_3;
            float2  uv_19 = (pix_center_3 + make_float2 (1.0f, -0.0f) - _S547) / _S548;
            FixedArray<float, 1>  _S552 = dist_coeffs_9;
            float2  uv_u_8;
            bool _S553 = undistort_point_0(uv_19, &_S552, int(12), &uv_u_8);
            if(!_S553)
            {
                _S546 = int(0);
                _S545 = nullptr;
                _S544 = &points_4[int(0)];
                _S543 = nullptr;
                _S542 = nullptr;
                _S541 = _S551;
                break;
            }
            float3  raydir_4 = unproject_raydir_0(uv_u_8, camera_model_8, is_ray_depth_7);
            points_4[int(1)] = make_float3 (depths_2.y) * raydir_4;
            float2  uv_20 = (pix_center_3 + make_float2 (0.0f, -1.0f) - _S547) / _S548;
            FixedArray<float, 1>  _S554 = dist_coeffs_9;
            float2  uv_u_9;
            bool _S555 = undistort_point_0(uv_20, &_S554, int(12), &uv_u_9);
            if(!_S555)
            {
                _S546 = int(0);
                _S545 = &points_4[int(1)];
                _S544 = &points_4[int(0)];
                _S543 = nullptr;
                _S542 = nullptr;
                _S541 = _S551;
                break;
            }
            float3  raydir_5 = unproject_raydir_0(uv_u_9, camera_model_8, is_ray_depth_7);
            points_4[int(2)] = make_float3 (depths_2.z) * raydir_5;
            float2  uv_21 = (pix_center_3 + make_float2 (0.0f, 1.0f) - _S547) / _S548;
            FixedArray<float, 1>  _S556 = dist_coeffs_9;
            float2  uv_u_10;
            bool _S557 = undistort_point_0(uv_21, &_S556, int(12), &uv_u_10);
            if(!_S557)
            {
                _S546 = int(0);
                _S545 = &points_4[int(1)];
                _S544 = &points_4[int(0)];
                _S543 = nullptr;
                _S542 = &points_4[int(2)];
                _S541 = _S551;
                break;
            }
            float3  raydir_6 = unproject_raydir_0(uv_u_10, camera_model_8, is_ray_depth_7);
            points_4[int(3)] = make_float3 (depths_2.w) * raydir_6;
            float2  uv_22 = (pix_center_3 + make_float2 (0.0f) * make_float2 (0.0f, 3.0f) - _S547) / _S548;
            FixedArray<float, 1>  _S558 = dist_coeffs_9;
            float2  uv_u_11;
            bool _S559 = undistort_point_0(uv_22, &_S558, int(12), &uv_u_11);
            if(!_S559)
            {
                _S546 = int(0);
                _S545 = &points_4[int(1)];
                _S544 = &points_4[int(0)];
                _S543 = &points_4[int(3)];
                _S542 = &points_4[int(2)];
                _S541 = _S551;
                break;
            }
            float3  raydir_7 = unproject_raydir_0(uv_u_11, camera_model_8, is_ray_depth_7);
            _S546 = int(1);
            _S545 = &points_4[int(1)];
            _S544 = &points_4[int(0)];
            _S543 = &points_4[int(3)];
            _S542 = &points_4[int(2)];
            _S541 = raydir_7;
            break;
        }
        if(_S546 != int(1))
        {
            _S540 = 0.0f;
            break;
        }
        float3  normal_4 = cross_0(*_S545 - *_S544, - (*_S543 - *_S542));
        float3  normal_5;
        if((dot_0(normal_4, normal_4)) != 0.0f)
        {
            normal_5 = normalize_0(normal_4);
        }
        else
        {
            normal_5 = normal_4;
        }
        float3  _S560;
        if((dot_0(gt_normal_0, gt_normal_0)) != 0.0f)
        {
            _S560 = normalize_0(gt_normal_0);
        }
        else
        {
            _S560 = gt_normal_0;
        }
        _S540 = (1.0f - dot_0(normal_5, _S560) + 0.00100000004749745f) / ((F32_max((dot_0(normal_5, - normalize_0(_S541))), (0.0f))) + 0.00100000004749745f);
        break;
    }
    return _S540;
}

struct s_bwd_prop_depth_normal_loss_Intermediates_0
{
    float2  _S561;
    bool _S562;
    float2  _S563;
    bool _S564;
    float2  _S565;
    bool _S566;
    float2  _S567;
    bool _S568;
    float2  _S569;
    bool _S570;
};

inline __device__ void s_bwd_prop_normalize_impl_0(DiffPair_vectorx3Cfloatx2C3x3E_0 * dpx_14, float3  _s_dOut_9)
{
    float _S571 = length_0((*dpx_14).primal_0);
    float3  _S572 = (*dpx_14).primal_0 * _s_dOut_9;
    float3  _S573 = make_float3 (1.0f / _S571) * _s_dOut_9;
    float _S574 = - ((_S572.x + _S572.y + _S572.z) / (_S571 * _S571));
    float3  _S575 = make_float3 (0.0f);
    DiffPair_vectorx3Cfloatx2C3x3E_0 _S576;
    (&_S576)->primal_0 = (*dpx_14).primal_0;
    (&_S576)->differential_0 = _S575;
    s_bwd_length_impl_0(&_S576, _S574);
    float3  _S577 = _S573 + _S576.differential_0;
    dpx_14->primal_0 = (*dpx_14).primal_0;
    dpx_14->differential_0 = _S577;
    return;
}

inline __device__ void s_bwd_normalize_impl_0(DiffPair_vectorx3Cfloatx2C3x3E_0 * _S578, float3  _S579)
{
    s_bwd_prop_normalize_impl_0(_S578, _S579);
    return;
}

inline __device__ void depth_normal_loss_vjp_none(float2  pix_center_4, float4  intrins_7, FixedArray<float, 1>  dist_coeffs_10, int camera_model_9, bool is_ray_depth_8, float4  depths_3, float3  gt_normal_1, float v_loss_0, float4  * v_depths_1, float3  * v_gt_normal_0)
{
    float2  _S580 = make_float2 (0.0f);
    s_bwd_prop_depth_normal_loss_Intermediates_0 _S581;
    (&_S581)->_S561 = _S580;
    (&_S581)->_S562 = false;
    (&_S581)->_S563 = _S580;
    (&_S581)->_S564 = false;
    (&_S581)->_S565 = _S580;
    (&_S581)->_S566 = false;
    (&_S581)->_S567 = _S580;
    (&_S581)->_S568 = false;
    (&_S581)->_S569 = _S580;
    (&_S581)->_S570 = false;
    (&_S581)->_S563 = _S580;
    (&_S581)->_S564 = false;
    (&_S581)->_S565 = _S580;
    (&_S581)->_S566 = false;
    (&_S581)->_S567 = _S580;
    (&_S581)->_S568 = false;
    (&_S581)->_S569 = _S580;
    (&_S581)->_S570 = false;
    float2  _S582 = float2 {intrins_7.z, intrins_7.w};
    float2  _S583 = float2 {intrins_7.x, intrins_7.y};
    float2  uv_23 = (pix_center_4 + make_float2 (-1.0f, -0.0f) - _S582) / _S583;
    float2  _S584 = _S580;
    FixedArray<float, 1>  _S585 = dist_coeffs_10;
    bool _S586 = undistort_point_0(uv_23, &_S585, int(12), &_S584);
    (&_S581)->_S561 = _S584;
    (&_S581)->_S562 = _S586;
    bool _S587 = !!_S586;
    bool _runFlag_7;
    if(_S587)
    {
        float2  uv_24 = (pix_center_4 + make_float2 (1.0f, -0.0f) - _S582) / _S583;
        float2  _S588 = _S580;
        FixedArray<float, 1>  _S589 = dist_coeffs_10;
        bool _S590 = undistort_point_0(uv_24, &_S589, int(12), &_S588);
        (&_S581)->_S563 = _S588;
        (&_S581)->_S564 = _S590;
        if(!_S590)
        {
            _runFlag_7 = false;
        }
        else
        {
            _runFlag_7 = _S587;
        }
        if(_runFlag_7)
        {
            float2  uv_25 = (pix_center_4 + make_float2 (0.0f, -1.0f) - _S582) / _S583;
            float2  _S591 = _S580;
            FixedArray<float, 1>  _S592 = dist_coeffs_10;
            bool _S593 = undistort_point_0(uv_25, &_S592, int(12), &_S591);
            (&_S581)->_S565 = _S591;
            (&_S581)->_S566 = _S593;
            if(!_S593)
            {
                _runFlag_7 = false;
            }
            if(_runFlag_7)
            {
                float2  uv_26 = (pix_center_4 + make_float2 (0.0f, 1.0f) - _S582) / _S583;
                float2  _S594 = _S580;
                FixedArray<float, 1>  _S595 = dist_coeffs_10;
                bool _S596 = undistort_point_0(uv_26, &_S595, int(12), &_S594);
                (&_S581)->_S567 = _S594;
                (&_S581)->_S568 = _S596;
                if(!_S596)
                {
                    _runFlag_7 = false;
                }
                if(_runFlag_7)
                {
                    float2  uv_27 = (pix_center_4 - _S582) / _S583;
                    float2  _S597 = _S580;
                    FixedArray<float, 1>  _S598 = dist_coeffs_10;
                    bool _S599 = undistort_point_0(uv_27, &_S598, int(12), &_S597);
                    (&_S581)->_S569 = _S597;
                    (&_S581)->_S570 = _S599;
                }
            }
        }
    }
    s_bwd_prop_depth_normal_loss_Intermediates_0 _S600 = _S581;
    float3  _S601 = make_float3 (0.0f);
    bool _S602 = !!_S581._S562;
    bool _runFlag_8;
    bool _runFlag_9;
    bool _runFlag_10;
    int _S603;
    float3  raydir_8;
    float3  _S604;
    float3  _S605;
    float3  _S606;
    float3  _S607;
    FixedArray<float3 , 5>  points_5;
    if(_S602)
    {
        float3  _S608 = s_primal_ctx_unproject_raydir_0(_S600._S561, camera_model_9, is_ray_depth_8);
        float3  _S609 = make_float3 (depths_3.x) * _S608;
        if(!_S600._S564)
        {
            _runFlag_7 = false;
        }
        else
        {
            _runFlag_7 = _S602;
        }
        if(_runFlag_7)
        {
            float3  _S610 = s_primal_ctx_unproject_raydir_0(_S600._S563, camera_model_9, is_ray_depth_8);
            float3  _S611 = make_float3 (depths_3.y) * _S610;
            if(!_S600._S566)
            {
                _runFlag_8 = false;
            }
            else
            {
                _runFlag_8 = _runFlag_7;
            }
            if(_runFlag_8)
            {
                float3  _S612 = s_primal_ctx_unproject_raydir_0(_S600._S565, camera_model_9, is_ray_depth_8);
                float3  _S613 = make_float3 (depths_3.z) * _S612;
                if(!_S600._S568)
                {
                    _runFlag_9 = false;
                }
                else
                {
                    _runFlag_9 = _runFlag_8;
                }
                if(_runFlag_9)
                {
                    float3  _S614 = s_primal_ctx_unproject_raydir_0(_S600._S567, camera_model_9, is_ray_depth_8);
                    float3  _S615 = make_float3 (depths_3.w) * _S614;
                    if(!_S600._S570)
                    {
                        _runFlag_10 = false;
                    }
                    else
                    {
                        _runFlag_10 = _runFlag_9;
                    }
                    if(_runFlag_10)
                    {
                        float3  _S616 = s_primal_ctx_unproject_raydir_0(_S600._S569, camera_model_9, is_ray_depth_8);
                        _S603 = int(1);
                        raydir_8 = _S616;
                    }
                    else
                    {
                        _S603 = int(0);
                        raydir_8 = _S614;
                    }
                    points_5[int(0)] = _S609;
                    points_5[int(1)] = _S611;
                    points_5[int(2)] = _S613;
                    points_5[int(3)] = _S615;
                    points_5[int(4)] = _S601;
                    _S604 = _S614;
                }
                else
                {
                    _S603 = int(0);
                    raydir_8 = _S612;
                    points_5[int(0)] = _S609;
                    points_5[int(1)] = _S611;
                    points_5[int(2)] = _S613;
                    points_5[int(3)] = _S601;
                    points_5[int(4)] = _S601;
                    _S604 = _S601;
                }
                _S605 = _S612;
            }
            else
            {
                _S603 = int(0);
                raydir_8 = _S610;
                points_5[int(0)] = _S609;
                points_5[int(1)] = _S611;
                points_5[int(2)] = _S601;
                points_5[int(3)] = _S601;
                points_5[int(4)] = _S601;
                _runFlag_9 = false;
                _S604 = _S601;
                _S605 = _S601;
            }
            _S606 = _S610;
        }
        else
        {
            _S603 = int(0);
            raydir_8 = _S608;
            points_5[int(0)] = _S609;
            points_5[int(1)] = _S601;
            points_5[int(2)] = _S601;
            points_5[int(3)] = _S601;
            points_5[int(4)] = _S601;
            _runFlag_8 = false;
            _runFlag_9 = false;
            _S604 = _S601;
            _S605 = _S601;
            _S606 = _S601;
        }
        _S607 = _S608;
    }
    else
    {
        _S603 = int(0);
        points_5[int(0)] = _S601;
        points_5[int(1)] = _S601;
        points_5[int(2)] = _S601;
        points_5[int(3)] = _S601;
        points_5[int(4)] = _S601;
        _runFlag_7 = false;
        _runFlag_8 = false;
        _runFlag_9 = false;
        _S604 = _S601;
        _S605 = _S601;
        _S606 = _S601;
        _S607 = _S601;
    }
    bool _S617 = !(_S603 != int(1));
    bool _S618;
    float3  normal_6;
    float3  _S619;
    float3  _S620;
    float3  _S621;
    float3  _S622;
    float _S623;
    float _S624;
    float _S625;
    float _S626;
    if(_S617)
    {
        float3  dx_2 = points_5[int(1)] - points_5[int(0)];
        float3  _S627 = - (points_5[int(3)] - points_5[int(2)]);
        float3  _S628 = s_primal_ctx_cross_0(dx_2, _S627);
        bool _S629 = (s_primal_ctx_dot_0(_S628, _S628)) != 0.0f;
        if(_S629)
        {
            normal_6 = normalize_0(_S628);
        }
        else
        {
            normal_6 = _S628;
        }
        bool _S630 = (s_primal_ctx_dot_0(gt_normal_1, gt_normal_1)) != 0.0f;
        if(_S630)
        {
            _S619 = normalize_0(gt_normal_1);
        }
        else
        {
            _S619 = gt_normal_1;
        }
        float3  _S631 = - normalize_0(raydir_8);
        float _S632 = s_primal_ctx_dot_0(normal_6, _S631);
        float _S633 = 1.0f - s_primal_ctx_dot_0(normal_6, _S619) + 0.00100000004749745f;
        float _S634 = (F32_max((_S632), (0.0f))) + 0.00100000004749745f;
        _S623 = _S634 * _S634;
        _S624 = _S633;
        _S625 = _S634;
        _S626 = _S632;
        raydir_8 = normal_6;
        normal_6 = _S631;
        _runFlag_10 = _S630;
        _S618 = _S629;
        _S620 = _S628;
        _S621 = dx_2;
        _S622 = _S627;
    }
    else
    {
        _S623 = 0.0f;
        _S624 = 0.0f;
        _S625 = 0.0f;
        _S626 = 0.0f;
        raydir_8 = _S601;
        normal_6 = _S601;
        _S619 = _S601;
        _runFlag_10 = false;
        _S618 = false;
        _S620 = _S601;
        _S621 = _S601;
        _S622 = _S601;
    }
    float4  _S635 = make_float4 (0.0f);
    if(_S617)
    {
        float _S636 = v_loss_0 / _S623;
        float _S637 = _S624 * - _S636;
        float s_diff_num_T_0 = _S625 * _S636;
        DiffPair_float_0 _S638;
        (&_S638)->primal_0 = _S626;
        (&_S638)->differential_0 = 0.0f;
        DiffPair_float_0 _S639;
        (&_S639)->primal_0 = 0.0f;
        (&_S639)->differential_0 = 0.0f;
        _d_max_0(&_S638, &_S639, _S637);
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S640;
        (&_S640)->primal_0 = raydir_8;
        (&_S640)->differential_0 = _S601;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S641;
        (&_S641)->primal_0 = normal_6;
        (&_S641)->differential_0 = _S601;
        s_bwd_prop_dot_0(&_S640, &_S641, _S638.differential_0);
        float _S642 = - s_diff_num_T_0;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S643;
        (&_S643)->primal_0 = raydir_8;
        (&_S643)->differential_0 = _S601;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S644;
        (&_S644)->primal_0 = _S619;
        (&_S644)->differential_0 = _S601;
        s_bwd_prop_dot_0(&_S643, &_S644, _S642);
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S645 = _S644;
        float3  _S646 = _S640.differential_0 + _S643.differential_0;
        if(_runFlag_10)
        {
            DiffPair_vectorx3Cfloatx2C3x3E_0 _S647;
            (&_S647)->primal_0 = gt_normal_1;
            (&_S647)->differential_0 = _S601;
            s_bwd_normalize_impl_0(&_S647, _S645.differential_0);
            raydir_8 = _S647.differential_0;
        }
        else
        {
            raydir_8 = _S645.differential_0;
        }
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S648;
        (&_S648)->primal_0 = gt_normal_1;
        (&_S648)->differential_0 = _S601;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S649;
        (&_S649)->primal_0 = gt_normal_1;
        (&_S649)->differential_0 = _S601;
        s_bwd_prop_dot_0(&_S648, &_S649, 0.0f);
        float3  _S650 = _S649.differential_0 + _S648.differential_0 + raydir_8;
        if(_S618)
        {
            DiffPair_vectorx3Cfloatx2C3x3E_0 _S651;
            (&_S651)->primal_0 = _S620;
            (&_S651)->differential_0 = _S601;
            s_bwd_normalize_impl_0(&_S651, _S646);
            raydir_8 = _S651.differential_0;
        }
        else
        {
            raydir_8 = _S646;
        }
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S652;
        (&_S652)->primal_0 = _S620;
        (&_S652)->differential_0 = _S601;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S653;
        (&_S653)->primal_0 = _S620;
        (&_S653)->differential_0 = _S601;
        s_bwd_prop_dot_0(&_S652, &_S653, 0.0f);
        float3  _S654 = _S653.differential_0 + _S652.differential_0 + raydir_8;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S655;
        (&_S655)->primal_0 = _S621;
        (&_S655)->differential_0 = _S601;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S656;
        (&_S656)->primal_0 = _S622;
        (&_S656)->differential_0 = _S601;
        s_bwd_prop_cross_0(&_S655, &_S656, _S654);
        float3  s_diff_dy_T_2 = - _S656.differential_0;
        float3  _S657 = - s_diff_dy_T_2;
        float3  _S658 = - _S655.differential_0;
        FixedArray<float3 , 5>  _S659;
        _S659[int(0)] = _S601;
        _S659[int(1)] = _S601;
        _S659[int(2)] = _S601;
        _S659[int(3)] = _S601;
        _S659[int(4)] = _S601;
        _S659[int(2)] = _S657;
        _S659[int(3)] = s_diff_dy_T_2;
        _S659[int(0)] = _S658;
        _S659[int(1)] = _S655.differential_0;
        points_5[int(0)] = _S659[int(0)];
        points_5[int(1)] = _S659[int(1)];
        points_5[int(2)] = _S659[int(2)];
        points_5[int(3)] = _S659[int(3)];
        points_5[int(4)] = _S659[int(4)];
        raydir_8 = _S650;
    }
    else
    {
        points_5[int(0)] = _S601;
        points_5[int(1)] = _S601;
        points_5[int(2)] = _S601;
        points_5[int(3)] = _S601;
        points_5[int(4)] = _S601;
        raydir_8 = _S601;
    }
    float4  _S660;
    if(_S602)
    {
        if(_runFlag_7)
        {
            if(_runFlag_8)
            {
                if(_runFlag_9)
                {
                    FixedArray<float3 , 5>  _S661 = points_5;
                    FixedArray<float3 , 5>  _S662 = points_5;
                    FixedArray<float3 , 5>  _S663 = points_5;
                    float3  _S664 = _S604 * points_5[int(3)];
                    float _S665 = _S664.x + _S664.y + _S664.z;
                    float4  _S666 = _S635;
                    *&((&_S666)->w) = _S665;
                    points_5[int(0)] = _S601;
                    points_5[int(1)] = _S601;
                    points_5[int(2)] = _S601;
                    points_5[int(3)] = _S601;
                    points_5[int(4)] = _S601;
                    _S604 = _S663[int(2)];
                    normal_6 = _S661[int(0)];
                    _S619 = _S662[int(1)];
                    _S660 = _S666;
                }
                else
                {
                    FixedArray<float3 , 5>  _S667 = points_5;
                    FixedArray<float3 , 5>  _S668 = points_5;
                    FixedArray<float3 , 5>  _S669 = points_5;
                    FixedArray<float3 , 5>  _S670 = points_5;
                    points_5[int(0)] = points_5[int(0)];
                    points_5[int(1)] = _S667[int(1)];
                    points_5[int(2)] = _S668[int(2)];
                    points_5[int(3)] = _S669[int(3)];
                    points_5[int(4)] = _S670[int(4)];
                    _S604 = _S601;
                    normal_6 = _S601;
                    _S619 = _S601;
                    _S660 = _S635;
                }
                float3  _S671 = _S605 * (points_5[int(2)] + _S604);
                float _S672 = _S671.x + _S671.y + _S671.z;
                float3  _S673 = points_5[int(0)] + normal_6;
                float3  _S674 = points_5[int(1)] + _S619;
                float4  _S675 = _S635;
                *&((&_S675)->z) = _S672;
                float4  _S676 = _S660 + _S675;
                points_5[int(0)] = _S601;
                points_5[int(1)] = _S601;
                points_5[int(2)] = _S601;
                points_5[int(3)] = _S601;
                points_5[int(4)] = _S601;
                _S604 = _S674;
                _S605 = _S673;
                _S660 = _S676;
            }
            else
            {
                FixedArray<float3 , 5>  _S677 = points_5;
                FixedArray<float3 , 5>  _S678 = points_5;
                FixedArray<float3 , 5>  _S679 = points_5;
                FixedArray<float3 , 5>  _S680 = points_5;
                points_5[int(0)] = points_5[int(0)];
                points_5[int(1)] = _S677[int(1)];
                points_5[int(2)] = _S678[int(2)];
                points_5[int(3)] = _S679[int(3)];
                points_5[int(4)] = _S680[int(4)];
                _S604 = _S601;
                _S605 = _S601;
                _S660 = _S635;
            }
            float3  _S681 = _S606 * (points_5[int(1)] + _S604);
            float _S682 = _S681.x + _S681.y + _S681.z;
            float3  _S683 = points_5[int(0)] + _S605;
            float4  _S684 = _S635;
            *&((&_S684)->y) = _S682;
            float4  _S685 = _S660 + _S684;
            points_5[int(0)] = _S601;
            points_5[int(1)] = _S601;
            points_5[int(2)] = _S601;
            points_5[int(3)] = _S601;
            points_5[int(4)] = _S601;
            _S604 = _S683;
            _S660 = _S685;
        }
        else
        {
            FixedArray<float3 , 5>  _S686 = points_5;
            FixedArray<float3 , 5>  _S687 = points_5;
            FixedArray<float3 , 5>  _S688 = points_5;
            FixedArray<float3 , 5>  _S689 = points_5;
            points_5[int(0)] = points_5[int(0)];
            points_5[int(1)] = _S686[int(1)];
            points_5[int(2)] = _S687[int(2)];
            points_5[int(3)] = _S688[int(3)];
            points_5[int(4)] = _S689[int(4)];
            _S604 = _S601;
            _S660 = _S635;
        }
        float3  _S690 = _S607 * (points_5[int(0)] + _S604);
        float _S691 = _S690.x + _S690.y + _S690.z;
        float4  _S692 = _S635;
        *&((&_S692)->x) = _S691;
        _S660 = _S660 + _S692;
    }
    else
    {
        _S660 = _S635;
    }
    *v_depths_1 = _S660;
    *v_gt_normal_0 = raydir_8;
    return;
}

inline __device__ float3  generate_ray_d2n_opencv(float2  pix_pos_3, float4  intrins_8, FixedArray<float, 4>  dist_coeffs_11, int camera_model_10, bool is_ray_depth_9)
{
    float3  _S693;
    for(;;)
    {
        float2  uv_28 = (pix_pos_3 - float2 {intrins_8.z, intrins_8.w}) / float2 {intrins_8.x, intrins_8.y};
        FixedArray<float, 4>  _S694 = dist_coeffs_11;
        float2  uv_u_12;
        bool _S695 = undistort_point_1(uv_28, &_S694, int(12), &uv_u_12);
        if(!_S695)
        {
            int3  _S696 = make_int3 (int(0));
            float3  _S697 = make_float3 ((float)_S696.x, (float)_S696.y, (float)_S696.z);
            _S693 = _S697;
            break;
        }
        _S693 = unproject_raydir_0(uv_u_12, camera_model_10, is_ray_depth_9);
        break;
    }
    return _S693;
}

inline __device__ float3  depth_to_point_opencv(float2  pix_pos_4, float4  intrins_9, FixedArray<float, 4>  dist_coeffs_12, int camera_model_11, bool is_ray_depth_10, float depth_4)
{
    float3  _S698;
    for(;;)
    {
        float2  uv_29 = (pix_pos_4 - float2 {intrins_9.z, intrins_9.w}) / float2 {intrins_9.x, intrins_9.y};
        FixedArray<float, 4>  _S699 = dist_coeffs_12;
        float2  uv_u_13;
        bool _S700 = undistort_point_1(uv_29, &_S699, int(12), &uv_u_13);
        if(!_S700)
        {
            _S698 = make_float3 (0.0f);
            break;
        }
        _S698 = make_float3 (depth_4) * unproject_raydir_0(uv_u_13, camera_model_11, is_ray_depth_10);
        break;
    }
    return _S698;
}

struct s_bwd_prop_depth_to_point_Intermediates_1
{
    float2  _S701;
    bool _S702;
};

inline __device__ float depth_to_point_vjp_opencv(float2  pix_pos_5, float4  intrins_10, FixedArray<float, 4>  dist_coeffs_13, int camera_model_12, bool is_ray_depth_11, float depth_5, float3  v_point_1)
{
    float2  _S703 = make_float2 (0.0f);
    s_bwd_prop_depth_to_point_Intermediates_1 _S704;
    (&_S704)->_S701 = _S703;
    (&_S704)->_S702 = false;
    float2  uv_30 = (pix_pos_5 - float2 {intrins_10.z, intrins_10.w}) / float2 {intrins_10.x, intrins_10.y};
    float2  _S705 = _S703;
    FixedArray<float, 4>  _S706 = dist_coeffs_13;
    bool _S707 = undistort_point_1(uv_30, &_S706, int(12), &_S705);
    (&_S704)->_S701 = _S705;
    (&_S704)->_S702 = _S707;
    s_bwd_prop_depth_to_point_Intermediates_1 _S708 = _S704;
    float3  _S709 = make_float3 (0.0f);
    bool _S710 = !!_S704._S702;
    float3  _S711;
    if(_S710)
    {
        _S711 = s_primal_ctx_unproject_raydir_0(_S708._S701, camera_model_12, is_ray_depth_11);
    }
    else
    {
        _S711 = _S709;
    }
    if(_S710)
    {
        _S711 = _S711 * v_point_1;
    }
    else
    {
        _S711 = _S709;
    }
    return _S711.x + _S711.y + _S711.z;
}

inline __device__ float3  depth_to_normal_opencv(float2  pix_center_5, float4  intrins_11, FixedArray<float, 4>  dist_coeffs_14, int camera_model_13, bool is_ray_depth_12, float4  depths_4)
{
    float3  normal_7;
    for(;;)
    {
        bool _S712;
        if((depths_4.x) == 0.0f)
        {
            _S712 = true;
        }
        else
        {
            _S712 = (depths_4.y) == 0.0f;
        }
        if(_S712)
        {
            _S712 = true;
        }
        else
        {
            _S712 = (depths_4.z) == 0.0f;
        }
        if(_S712)
        {
            _S712 = true;
        }
        else
        {
            _S712 = (depths_4.w) == 0.0f;
        }
        if(_S712)
        {
            normal_7 = make_float3 (0.0f);
            break;
        }
        float3  * _S713;
        float3  * _S714;
        float3  * _S715;
        float3  * _S716;
        int _S717;
        FixedArray<float3 , 4>  points_6;
        for(;;)
        {
            float2  _S718 = float2 {intrins_11.z, intrins_11.w};
            float2  _S719 = float2 {intrins_11.x, intrins_11.y};
            float2  uv_31 = (pix_center_5 + make_float2 (-1.0f, -0.0f) - _S718) / _S719;
            FixedArray<float, 4>  _S720 = dist_coeffs_14;
            float2  uv_u_14;
            bool _S721 = undistort_point_1(uv_31, &_S720, int(12), &uv_u_14);
            if(!_S721)
            {
                float3  _S722 = make_float3 (0.0f);
                _S717 = int(0);
                _S716 = nullptr;
                _S715 = nullptr;
                _S714 = nullptr;
                _S713 = nullptr;
                normal_7 = _S722;
                break;
            }
            points_6[int(0)] = make_float3 (depths_4.x) * unproject_raydir_0(uv_u_14, camera_model_13, is_ray_depth_12);
            for(;;)
            {
                float2  uv_32 = (pix_center_5 + make_float2 (1.0f, -0.0f) - _S718) / _S719;
                FixedArray<float, 4>  _S723 = dist_coeffs_14;
                float2  uv_u_15;
                bool _S724 = undistort_point_1(uv_32, &_S723, int(12), &uv_u_15);
                if(!_S724)
                {
                    float3  _S725 = make_float3 (0.0f);
                    _S717 = int(0);
                    _S716 = nullptr;
                    normal_7 = _S725;
                    break;
                }
                points_6[int(1)] = make_float3 (depths_4.y) * unproject_raydir_0(uv_u_15, camera_model_13, is_ray_depth_12);
                _S717 = int(2);
                _S716 = &points_6[int(1)];
                break;
            }
            if(_S717 != int(2))
            {
                _S715 = &points_6[int(0)];
                _S714 = nullptr;
                _S713 = nullptr;
                break;
            }
            float2  uv_33 = (pix_center_5 + make_float2 (0.0f, -1.0f) - _S718) / _S719;
            FixedArray<float, 4>  _S726 = dist_coeffs_14;
            float2  uv_u_16;
            bool _S727 = undistort_point_1(uv_33, &_S726, int(12), &uv_u_16);
            if(!_S727)
            {
                float3  _S728 = make_float3 (0.0f);
                _S717 = int(0);
                _S715 = &points_6[int(0)];
                _S714 = nullptr;
                _S713 = nullptr;
                normal_7 = _S728;
                break;
            }
            points_6[int(2)] = make_float3 (depths_4.z) * unproject_raydir_0(uv_u_16, camera_model_13, is_ray_depth_12);
            for(;;)
            {
                float2  uv_34 = (pix_center_5 + make_float2 (0.0f, 1.0f) - _S718) / _S719;
                FixedArray<float, 4>  _S729 = dist_coeffs_14;
                float2  uv_u_17;
                bool _S730 = undistort_point_1(uv_34, &_S729, int(12), &uv_u_17);
                if(!_S730)
                {
                    float3  _S731 = make_float3 (0.0f);
                    _S717 = int(0);
                    _S715 = nullptr;
                    normal_7 = _S731;
                    break;
                }
                points_6[int(3)] = make_float3 (depths_4.w) * unproject_raydir_0(uv_u_17, camera_model_13, is_ray_depth_12);
                _S717 = int(2);
                _S715 = &points_6[int(3)];
                break;
            }
            if(_S717 != int(2))
            {
                float3  * _S732 = _S715;
                _S715 = &points_6[int(0)];
                _S714 = _S732;
                _S713 = &points_6[int(2)];
                break;
            }
            float3  * _S733 = _S715;
            _S717 = int(1);
            _S715 = &points_6[int(0)];
            _S714 = _S733;
            _S713 = &points_6[int(2)];
            break;
        }
        if(_S717 != int(1))
        {
            break;
        }
        float3  normal_8 = cross_0(*_S716 - *_S715, - (*_S714 - *_S713));
        if((dot_0(normal_8, normal_8)) != 0.0f)
        {
            normal_7 = normal_8 / make_float3 (length_0(normal_8));
        }
        else
        {
            normal_7 = normal_8;
        }
        break;
    }
    return normal_7;
}

struct s_bwd_prop_depth_to_normal_Intermediates_1
{
    float2  _S734;
    bool _S735;
    float2  _S736;
    bool _S737;
    float2  _S738;
    bool _S739;
    float2  _S740;
    bool _S741;
};

inline __device__ void depth_to_normal_vjp_opencv(float2  pix_center_6, float4  intrins_12, FixedArray<float, 4>  dist_coeffs_15, int camera_model_14, bool is_ray_depth_13, float4  depths_5, float3  v_normal_2, float4  * v_depths_2)
{
    float2  _S742 = make_float2 (0.0f);
    s_bwd_prop_depth_to_normal_Intermediates_1 _S743;
    (&_S743)->_S734 = _S742;
    (&_S743)->_S735 = false;
    (&_S743)->_S736 = _S742;
    (&_S743)->_S737 = false;
    (&_S743)->_S738 = _S742;
    (&_S743)->_S739 = false;
    (&_S743)->_S740 = _S742;
    (&_S743)->_S741 = false;
    (&_S743)->_S734 = _S742;
    (&_S743)->_S735 = false;
    (&_S743)->_S736 = _S742;
    (&_S743)->_S737 = false;
    (&_S743)->_S738 = _S742;
    (&_S743)->_S739 = false;
    (&_S743)->_S740 = _S742;
    (&_S743)->_S741 = false;
    bool _S744 = (depths_5.x) == 0.0f;
    bool _runFlag_11;
    if(_S744)
    {
        _runFlag_11 = true;
    }
    else
    {
        _runFlag_11 = (depths_5.y) == 0.0f;
    }
    if(_runFlag_11)
    {
        _runFlag_11 = true;
    }
    else
    {
        _runFlag_11 = (depths_5.z) == 0.0f;
    }
    if(_runFlag_11)
    {
        _runFlag_11 = true;
    }
    else
    {
        _runFlag_11 = (depths_5.w) == 0.0f;
    }
    int _S745;
    if(!_runFlag_11)
    {
        float2  _S746 = float2 {intrins_12.z, intrins_12.w};
        float2  _S747 = float2 {intrins_12.x, intrins_12.y};
        float2  uv_35 = (pix_center_6 + make_float2 (-1.0f, -0.0f) - _S746) / _S747;
        float2  _S748 = _S742;
        FixedArray<float, 4>  _S749 = dist_coeffs_15;
        bool _S750 = undistort_point_1(uv_35, &_S749, int(12), &_S748);
        (&_S743)->_S734 = _S748;
        (&_S743)->_S735 = _S750;
        bool _S751 = !!_S750;
        if(_S751)
        {
            float2  uv_36 = (pix_center_6 + make_float2 (1.0f, -0.0f) - _S746) / _S747;
            float2  _S752 = _S742;
            FixedArray<float, 4>  _S753 = dist_coeffs_15;
            bool _S754 = undistort_point_1(uv_36, &_S753, int(12), &_S752);
            (&_S743)->_S736 = _S752;
            (&_S743)->_S737 = _S754;
            if(!!_S754)
            {
                _S745 = int(2);
            }
            else
            {
                _S745 = int(0);
            }
            if(_S745 != int(2))
            {
                _runFlag_11 = false;
            }
            else
            {
                _runFlag_11 = _S751;
            }
            if(_runFlag_11)
            {
                float2  uv_37 = (pix_center_6 + make_float2 (0.0f, -1.0f) - _S746) / _S747;
                float2  _S755 = _S742;
                FixedArray<float, 4>  _S756 = dist_coeffs_15;
                bool _S757 = undistort_point_1(uv_37, &_S756, int(12), &_S755);
                (&_S743)->_S738 = _S755;
                (&_S743)->_S739 = _S757;
                if(!_S757)
                {
                    _runFlag_11 = false;
                }
                if(_runFlag_11)
                {
                    float2  uv_38 = (pix_center_6 + make_float2 (0.0f, 1.0f) - _S746) / _S747;
                    float2  _S758 = _S742;
                    FixedArray<float, 4>  _S759 = dist_coeffs_15;
                    bool _S760 = undistort_point_1(uv_38, &_S759, int(12), &_S758);
                    (&_S743)->_S740 = _S758;
                    (&_S743)->_S741 = _S760;
                }
            }
        }
    }
    s_bwd_prop_depth_to_normal_Intermediates_1 _S761 = _S743;
    float3  _S762 = make_float3 (0.0f);
    if(_S744)
    {
        _runFlag_11 = true;
    }
    else
    {
        _runFlag_11 = (depths_5.y) == 0.0f;
    }
    if(_runFlag_11)
    {
        _runFlag_11 = true;
    }
    else
    {
        _runFlag_11 = (depths_5.z) == 0.0f;
    }
    if(_runFlag_11)
    {
        _runFlag_11 = true;
    }
    else
    {
        _runFlag_11 = (depths_5.w) == 0.0f;
    }
    bool _S763 = !_runFlag_11;
    bool _runFlag_12;
    bool _runFlag_13;
    bool _S764;
    bool _runFlag_14;
    bool _S765;
    bool _S766;
    FixedArray<float3 , 4>  points_7;
    float3  _S767;
    float3  _S768;
    float3  _S769;
    float3  _S770;
    float3  _S771;
    float3  _S772;
    float3  _S773;
    float3  _S774;
    float3  _S775;
    if(_S763)
    {
        bool _S776 = !!_S761._S735;
        if(_S776)
        {
            float3  _S777 = s_primal_ctx_unproject_raydir_0(_S761._S734, camera_model_14, is_ray_depth_13);
            float3  _S778 = make_float3 (depths_5.x) * _S777;
            bool _S779 = !!_S761._S737;
            if(_S779)
            {
                float3  _S780 = s_primal_ctx_unproject_raydir_0(_S761._S736, camera_model_14, is_ray_depth_13);
                float3  _S781 = make_float3 (depths_5.y) * _S780;
                _S745 = int(2);
                points_7[int(0)] = _S778;
                points_7[int(1)] = _S781;
                points_7[int(2)] = _S762;
                points_7[int(3)] = _S762;
                _S767 = _S780;
            }
            else
            {
                _S745 = int(0);
                points_7[int(0)] = _S778;
                points_7[int(1)] = _S762;
                points_7[int(2)] = _S762;
                points_7[int(3)] = _S762;
                _S767 = _S762;
            }
            if(_S745 != int(2))
            {
                _runFlag_11 = false;
            }
            else
            {
                _runFlag_11 = _S776;
                _S745 = int(0);
            }
            if(_runFlag_11)
            {
                if(!_S761._S739)
                {
                    _runFlag_12 = false;
                    _S745 = int(0);
                }
                else
                {
                    _runFlag_12 = _runFlag_11;
                }
                if(_runFlag_12)
                {
                    float3  _S782 = s_primal_ctx_unproject_raydir_0(_S761._S738, camera_model_14, is_ray_depth_13);
                    points_7[int(2)] = make_float3 (depths_5.z) * _S782;
                    bool _S783 = !!_S761._S741;
                    int _S784;
                    if(_S783)
                    {
                        float3  _S785 = s_primal_ctx_unproject_raydir_0(_S761._S740, camera_model_14, is_ray_depth_13);
                        points_7[int(3)] = make_float3 (depths_5.w) * _S785;
                        _S784 = int(2);
                        _S768 = _S785;
                    }
                    else
                    {
                        _S784 = int(0);
                        _S768 = _S762;
                    }
                    if(_S784 != int(2))
                    {
                        _runFlag_13 = false;
                        _S745 = _S784;
                    }
                    else
                    {
                        _runFlag_13 = _runFlag_12;
                    }
                    if(_runFlag_13)
                    {
                        _S745 = int(1);
                    }
                    _runFlag_13 = _S783;
                    _S769 = _S782;
                }
                else
                {
                    _runFlag_13 = false;
                    _S768 = _S762;
                    _S769 = _S762;
                }
            }
            else
            {
                _runFlag_12 = false;
                _runFlag_13 = false;
                _S768 = _S762;
                _S769 = _S762;
            }
            float3  _S786 = _S767;
            _S767 = _S768;
            _S768 = _S769;
            _S764 = _S779;
            _S769 = _S786;
            _S770 = _S777;
        }
        else
        {
            _S745 = int(0);
            points_7[int(0)] = _S762;
            points_7[int(1)] = _S762;
            points_7[int(2)] = _S762;
            points_7[int(3)] = _S762;
            _runFlag_11 = false;
            _runFlag_12 = false;
            _runFlag_13 = false;
            _S767 = _S762;
            _S768 = _S762;
            _S764 = false;
            _S769 = _S762;
            _S770 = _S762;
        }
        if(_S745 != int(1))
        {
            _runFlag_14 = false;
        }
        else
        {
            _runFlag_14 = _S763;
        }
        if(_runFlag_14)
        {
            float3  dx_3 = points_7[int(1)] - points_7[int(0)];
            float3  _S787 = - (points_7[int(3)] - points_7[int(2)]);
            float3  _S788 = s_primal_ctx_cross_0(dx_3, _S787);
            bool _S789 = (s_primal_ctx_dot_0(_S788, _S788)) != 0.0f;
            if(_S789)
            {
                float _S790 = length_0(_S788);
                float3  _S791 = make_float3 (_S790);
                _S771 = make_float3 (_S790 * _S790);
                _S772 = _S791;
            }
            else
            {
                _S771 = _S762;
                _S772 = _S762;
            }
            float3  _S792 = _S772;
            _S765 = _S789;
            _S772 = _S788;
            _S773 = _S792;
            _S774 = dx_3;
            _S775 = _S787;
        }
        else
        {
            _S765 = false;
            _S771 = _S762;
            _S772 = _S762;
            _S773 = _S762;
            _S774 = _S762;
            _S775 = _S762;
        }
        bool _S793 = _runFlag_11;
        bool _S794 = _runFlag_12;
        bool _S795 = _runFlag_13;
        float3  _S796 = _S767;
        float3  _S797 = _S768;
        bool _S798 = _S764;
        float3  _S799 = _S769;
        float3  _S800 = _S770;
        _runFlag_11 = _runFlag_14;
        _runFlag_12 = _S765;
        _S767 = _S771;
        _S768 = _S772;
        _S769 = _S773;
        _S770 = _S774;
        _S771 = _S775;
        _runFlag_13 = _S776;
        _S764 = _S793;
        _runFlag_14 = _S794;
        _S765 = _S795;
        _S772 = _S796;
        _S773 = _S797;
        _S766 = _S798;
        _S774 = _S799;
        _S775 = _S800;
    }
    else
    {
        _runFlag_11 = false;
        _runFlag_12 = false;
        _S767 = _S762;
        _S768 = _S762;
        _S769 = _S762;
        _S770 = _S762;
        _S771 = _S762;
        _runFlag_13 = false;
        _S764 = false;
        _runFlag_14 = false;
        _S765 = false;
        _S772 = _S762;
        _S773 = _S762;
        _S766 = false;
        _S774 = _S762;
        _S775 = _S762;
    }
    float4  _S801 = make_float4 (0.0f);
    float4  _S802;
    if(_S763)
    {
        if(_runFlag_11)
        {
            if(_runFlag_12)
            {
                float3  _S803 = v_normal_2 / _S767;
                float3  _S804 = _S768 * - _S803;
                float3  _S805 = _S769 * _S803;
                float _S806 = _S804.x + _S804.y + _S804.z;
                DiffPair_vectorx3Cfloatx2C3x3E_0 _S807;
                (&_S807)->primal_0 = _S768;
                (&_S807)->differential_0 = _S762;
                s_bwd_length_impl_0(&_S807, _S806);
                _S767 = _S805 + _S807.differential_0;
            }
            else
            {
                _S767 = v_normal_2;
            }
            DiffPair_vectorx3Cfloatx2C3x3E_0 _S808;
            (&_S808)->primal_0 = _S768;
            (&_S808)->differential_0 = _S762;
            DiffPair_vectorx3Cfloatx2C3x3E_0 _S809;
            (&_S809)->primal_0 = _S768;
            (&_S809)->differential_0 = _S762;
            s_bwd_prop_dot_0(&_S808, &_S809, 0.0f);
            float3  _S810 = _S809.differential_0 + _S808.differential_0 + _S767;
            DiffPair_vectorx3Cfloatx2C3x3E_0 _S811;
            (&_S811)->primal_0 = _S770;
            (&_S811)->differential_0 = _S762;
            DiffPair_vectorx3Cfloatx2C3x3E_0 _S812;
            (&_S812)->primal_0 = _S771;
            (&_S812)->differential_0 = _S762;
            s_bwd_prop_cross_0(&_S811, &_S812, _S810);
            float3  s_diff_dy_T_3 = - _S812.differential_0;
            float3  _S813 = - s_diff_dy_T_3;
            float3  _S814 = - _S811.differential_0;
            FixedArray<float3 , 4>  _S815;
            _S815[int(0)] = _S762;
            _S815[int(1)] = _S762;
            _S815[int(2)] = _S762;
            _S815[int(3)] = _S762;
            _S815[int(2)] = _S813;
            _S815[int(3)] = s_diff_dy_T_3;
            _S815[int(0)] = _S814;
            _S815[int(1)] = _S811.differential_0;
            points_7[int(0)] = _S815[int(0)];
            points_7[int(1)] = _S815[int(1)];
            points_7[int(2)] = _S815[int(2)];
            points_7[int(3)] = _S815[int(3)];
        }
        else
        {
            points_7[int(0)] = _S762;
            points_7[int(1)] = _S762;
            points_7[int(2)] = _S762;
            points_7[int(3)] = _S762;
        }
        if(_runFlag_13)
        {
            if(_S764)
            {
                if(_runFlag_14)
                {
                    FixedArray<float3 , 4>  _S816 = points_7;
                    FixedArray<float3 , 4>  _S817 = points_7;
                    FixedArray<float3 , 4>  _S818 = points_7;
                    FixedArray<float3 , 4>  _S819 = points_7;
                    if(_S765)
                    {
                        float3  _S820 = _S772 * _S819[int(3)];
                        float _S821 = _S820.x + _S820.y + _S820.z;
                        float4  _S822 = _S801;
                        *&((&_S822)->w) = _S821;
                        points_7[int(0)] = _S816[int(0)];
                        points_7[int(1)] = _S817[int(1)];
                        points_7[int(2)] = _S818[int(2)];
                        points_7[int(3)] = _S762;
                        _S802 = _S822;
                    }
                    else
                    {
                        points_7[int(0)] = _S816[int(0)];
                        points_7[int(1)] = _S817[int(1)];
                        points_7[int(2)] = _S818[int(2)];
                        points_7[int(3)] = _S819[int(3)];
                        _S802 = _S801;
                    }
                    float3  _S823 = _S773 * points_7[int(2)];
                    float _S824 = _S823.x + _S823.y + _S823.z;
                    FixedArray<float3 , 4>  _S825 = points_7;
                    FixedArray<float3 , 4>  _S826 = points_7;
                    float4  _S827 = _S801;
                    *&((&_S827)->z) = _S824;
                    float4  _S828 = _S802 + _S827;
                    points_7[int(0)] = points_7[int(0)];
                    points_7[int(1)] = _S825[int(1)];
                    points_7[int(2)] = _S762;
                    points_7[int(3)] = _S826[int(3)];
                    _S802 = _S828;
                }
                else
                {
                    FixedArray<float3 , 4>  _S829 = points_7;
                    FixedArray<float3 , 4>  _S830 = points_7;
                    FixedArray<float3 , 4>  _S831 = points_7;
                    points_7[int(0)] = points_7[int(0)];
                    points_7[int(1)] = _S829[int(1)];
                    points_7[int(2)] = _S830[int(2)];
                    points_7[int(3)] = _S831[int(3)];
                    _S802 = _S801;
                }
            }
            else
            {
                FixedArray<float3 , 4>  _S832 = points_7;
                FixedArray<float3 , 4>  _S833 = points_7;
                FixedArray<float3 , 4>  _S834 = points_7;
                points_7[int(0)] = points_7[int(0)];
                points_7[int(1)] = _S832[int(1)];
                points_7[int(2)] = _S833[int(2)];
                points_7[int(3)] = _S834[int(3)];
                _S802 = _S801;
            }
            if(_S766)
            {
                FixedArray<float3 , 4>  _S835 = points_7;
                float3  _S836 = _S774 * points_7[int(1)];
                float _S837 = _S836.x + _S836.y + _S836.z;
                float4  _S838 = _S801;
                *&((&_S838)->y) = _S837;
                float4  _S839 = _S802 + _S838;
                points_7[int(0)] = _S762;
                points_7[int(1)] = _S762;
                points_7[int(2)] = _S762;
                points_7[int(3)] = _S762;
                _S767 = _S835[int(0)];
                _S802 = _S839;
            }
            else
            {
                FixedArray<float3 , 4>  _S840 = points_7;
                FixedArray<float3 , 4>  _S841 = points_7;
                FixedArray<float3 , 4>  _S842 = points_7;
                points_7[int(0)] = points_7[int(0)];
                points_7[int(1)] = _S840[int(1)];
                points_7[int(2)] = _S841[int(2)];
                points_7[int(3)] = _S842[int(3)];
                _S767 = _S762;
            }
            float3  _S843 = _S775 * (points_7[int(0)] + _S767);
            float _S844 = _S843.x + _S843.y + _S843.z;
            float4  _S845 = _S801;
            *&((&_S845)->x) = _S844;
            _S802 = _S802 + _S845;
        }
        else
        {
            _S802 = _S801;
        }
    }
    else
    {
        _S802 = _S801;
    }
    *v_depths_2 = _S802;
    return;
}

inline __device__ float ray_depth_to_linear_depth_factor_opencv(float2  pix_center_7, float4  intrins_13, FixedArray<float, 4>  dist_coeffs_16, int camera_model_15)
{
    float _S846;
    for(;;)
    {
        float2  uv_39 = (pix_center_7 - float2 {intrins_13.z, intrins_13.w}) / float2 {intrins_13.x, intrins_13.y};
        FixedArray<float, 4>  _S847 = dist_coeffs_16;
        float2  uv_u_18;
        bool _S848 = undistort_point_1(uv_39, &_S847, int(12), &uv_u_18);
        if(!_S848)
        {
            _S846 = 0.0f;
            break;
        }
        float3  raydir_9 = unproject_raydir_0(uv_u_18, camera_model_15, false);
        _S846 = float((F32_sign((raydir_9.z)))) / length_0(raydir_9);
        break;
    }
    return _S846;
}

inline __device__ float depth_normal_loss_opencv(float2  pix_center_8, float4  intrins_14, FixedArray<float, 4>  dist_coeffs_17, int camera_model_16, bool is_ray_depth_14, float4  depths_6, float3  gt_normal_2)
{
    float _S849;
    for(;;)
    {
        float3  _S850;
        float3  * _S851;
        float3  * _S852;
        float3  * _S853;
        float3  * _S854;
        int _S855;
        FixedArray<float3 , 5>  points_8;
        for(;;)
        {
            float2  _S856 = float2 {intrins_14.z, intrins_14.w};
            float2  _S857 = float2 {intrins_14.x, intrins_14.y};
            float2  uv_40 = (pix_center_8 + make_float2 (-1.0f, -0.0f) - _S856) / _S857;
            FixedArray<float, 4>  _S858 = dist_coeffs_17;
            float2  uv_u_19;
            bool _S859 = undistort_point_1(uv_40, &_S858, int(12), &uv_u_19);
            float3  _S860 = make_float3 (0.0f);
            if(!_S859)
            {
                _S855 = int(0);
                _S854 = nullptr;
                _S853 = nullptr;
                _S852 = nullptr;
                _S851 = nullptr;
                _S850 = _S860;
                break;
            }
            float3  raydir_10 = unproject_raydir_0(uv_u_19, camera_model_16, is_ray_depth_14);
            points_8[int(0)] = make_float3 (depths_6.x) * raydir_10;
            float2  uv_41 = (pix_center_8 + make_float2 (1.0f, -0.0f) - _S856) / _S857;
            FixedArray<float, 4>  _S861 = dist_coeffs_17;
            float2  uv_u_20;
            bool _S862 = undistort_point_1(uv_41, &_S861, int(12), &uv_u_20);
            if(!_S862)
            {
                _S855 = int(0);
                _S854 = nullptr;
                _S853 = &points_8[int(0)];
                _S852 = nullptr;
                _S851 = nullptr;
                _S850 = _S860;
                break;
            }
            float3  raydir_11 = unproject_raydir_0(uv_u_20, camera_model_16, is_ray_depth_14);
            points_8[int(1)] = make_float3 (depths_6.y) * raydir_11;
            float2  uv_42 = (pix_center_8 + make_float2 (0.0f, -1.0f) - _S856) / _S857;
            FixedArray<float, 4>  _S863 = dist_coeffs_17;
            float2  uv_u_21;
            bool _S864 = undistort_point_1(uv_42, &_S863, int(12), &uv_u_21);
            if(!_S864)
            {
                _S855 = int(0);
                _S854 = &points_8[int(1)];
                _S853 = &points_8[int(0)];
                _S852 = nullptr;
                _S851 = nullptr;
                _S850 = _S860;
                break;
            }
            float3  raydir_12 = unproject_raydir_0(uv_u_21, camera_model_16, is_ray_depth_14);
            points_8[int(2)] = make_float3 (depths_6.z) * raydir_12;
            float2  uv_43 = (pix_center_8 + make_float2 (0.0f, 1.0f) - _S856) / _S857;
            FixedArray<float, 4>  _S865 = dist_coeffs_17;
            float2  uv_u_22;
            bool _S866 = undistort_point_1(uv_43, &_S865, int(12), &uv_u_22);
            if(!_S866)
            {
                _S855 = int(0);
                _S854 = &points_8[int(1)];
                _S853 = &points_8[int(0)];
                _S852 = nullptr;
                _S851 = &points_8[int(2)];
                _S850 = _S860;
                break;
            }
            float3  raydir_13 = unproject_raydir_0(uv_u_22, camera_model_16, is_ray_depth_14);
            points_8[int(3)] = make_float3 (depths_6.w) * raydir_13;
            float2  uv_44 = (pix_center_8 + make_float2 (0.0f) * make_float2 (0.0f, 3.0f) - _S856) / _S857;
            FixedArray<float, 4>  _S867 = dist_coeffs_17;
            float2  uv_u_23;
            bool _S868 = undistort_point_1(uv_44, &_S867, int(12), &uv_u_23);
            if(!_S868)
            {
                _S855 = int(0);
                _S854 = &points_8[int(1)];
                _S853 = &points_8[int(0)];
                _S852 = &points_8[int(3)];
                _S851 = &points_8[int(2)];
                _S850 = _S860;
                break;
            }
            float3  raydir_14 = unproject_raydir_0(uv_u_23, camera_model_16, is_ray_depth_14);
            _S855 = int(1);
            _S854 = &points_8[int(1)];
            _S853 = &points_8[int(0)];
            _S852 = &points_8[int(3)];
            _S851 = &points_8[int(2)];
            _S850 = raydir_14;
            break;
        }
        if(_S855 != int(1))
        {
            _S849 = 0.0f;
            break;
        }
        float3  normal_9 = cross_0(*_S854 - *_S853, - (*_S852 - *_S851));
        float3  normal_10;
        if((dot_0(normal_9, normal_9)) != 0.0f)
        {
            normal_10 = normalize_0(normal_9);
        }
        else
        {
            normal_10 = normal_9;
        }
        float3  _S869;
        if((dot_0(gt_normal_2, gt_normal_2)) != 0.0f)
        {
            _S869 = normalize_0(gt_normal_2);
        }
        else
        {
            _S869 = gt_normal_2;
        }
        _S849 = (1.0f - dot_0(normal_10, _S869) + 0.00100000004749745f) / ((F32_max((dot_0(normal_10, - normalize_0(_S850))), (0.0f))) + 0.00100000004749745f);
        break;
    }
    return _S849;
}

struct s_bwd_prop_depth_normal_loss_Intermediates_1
{
    float2  _S870;
    bool _S871;
    float2  _S872;
    bool _S873;
    float2  _S874;
    bool _S875;
    float2  _S876;
    bool _S877;
    float2  _S878;
    bool _S879;
};

inline __device__ void depth_normal_loss_vjp_opencv(float2  pix_center_9, float4  intrins_15, FixedArray<float, 4>  dist_coeffs_18, int camera_model_17, bool is_ray_depth_15, float4  depths_7, float3  gt_normal_3, float v_loss_1, float4  * v_depths_3, float3  * v_gt_normal_1)
{
    float2  _S880 = make_float2 (0.0f);
    s_bwd_prop_depth_normal_loss_Intermediates_1 _S881;
    (&_S881)->_S870 = _S880;
    (&_S881)->_S871 = false;
    (&_S881)->_S872 = _S880;
    (&_S881)->_S873 = false;
    (&_S881)->_S874 = _S880;
    (&_S881)->_S875 = false;
    (&_S881)->_S876 = _S880;
    (&_S881)->_S877 = false;
    (&_S881)->_S878 = _S880;
    (&_S881)->_S879 = false;
    (&_S881)->_S872 = _S880;
    (&_S881)->_S873 = false;
    (&_S881)->_S874 = _S880;
    (&_S881)->_S875 = false;
    (&_S881)->_S876 = _S880;
    (&_S881)->_S877 = false;
    (&_S881)->_S878 = _S880;
    (&_S881)->_S879 = false;
    float2  _S882 = float2 {intrins_15.z, intrins_15.w};
    float2  _S883 = float2 {intrins_15.x, intrins_15.y};
    float2  uv_45 = (pix_center_9 + make_float2 (-1.0f, -0.0f) - _S882) / _S883;
    float2  _S884 = _S880;
    FixedArray<float, 4>  _S885 = dist_coeffs_18;
    bool _S886 = undistort_point_1(uv_45, &_S885, int(12), &_S884);
    (&_S881)->_S870 = _S884;
    (&_S881)->_S871 = _S886;
    bool _S887 = !!_S886;
    bool _runFlag_15;
    if(_S887)
    {
        float2  uv_46 = (pix_center_9 + make_float2 (1.0f, -0.0f) - _S882) / _S883;
        float2  _S888 = _S880;
        FixedArray<float, 4>  _S889 = dist_coeffs_18;
        bool _S890 = undistort_point_1(uv_46, &_S889, int(12), &_S888);
        (&_S881)->_S872 = _S888;
        (&_S881)->_S873 = _S890;
        if(!_S890)
        {
            _runFlag_15 = false;
        }
        else
        {
            _runFlag_15 = _S887;
        }
        if(_runFlag_15)
        {
            float2  uv_47 = (pix_center_9 + make_float2 (0.0f, -1.0f) - _S882) / _S883;
            float2  _S891 = _S880;
            FixedArray<float, 4>  _S892 = dist_coeffs_18;
            bool _S893 = undistort_point_1(uv_47, &_S892, int(12), &_S891);
            (&_S881)->_S874 = _S891;
            (&_S881)->_S875 = _S893;
            if(!_S893)
            {
                _runFlag_15 = false;
            }
            if(_runFlag_15)
            {
                float2  uv_48 = (pix_center_9 + make_float2 (0.0f, 1.0f) - _S882) / _S883;
                float2  _S894 = _S880;
                FixedArray<float, 4>  _S895 = dist_coeffs_18;
                bool _S896 = undistort_point_1(uv_48, &_S895, int(12), &_S894);
                (&_S881)->_S876 = _S894;
                (&_S881)->_S877 = _S896;
                if(!_S896)
                {
                    _runFlag_15 = false;
                }
                if(_runFlag_15)
                {
                    float2  uv_49 = (pix_center_9 - _S882) / _S883;
                    float2  _S897 = _S880;
                    FixedArray<float, 4>  _S898 = dist_coeffs_18;
                    bool _S899 = undistort_point_1(uv_49, &_S898, int(12), &_S897);
                    (&_S881)->_S878 = _S897;
                    (&_S881)->_S879 = _S899;
                }
            }
        }
    }
    s_bwd_prop_depth_normal_loss_Intermediates_1 _S900 = _S881;
    float3  _S901 = make_float3 (0.0f);
    bool _S902 = !!_S881._S871;
    bool _runFlag_16;
    bool _runFlag_17;
    bool _runFlag_18;
    int _S903;
    float3  raydir_15;
    float3  _S904;
    float3  _S905;
    float3  _S906;
    float3  _S907;
    FixedArray<float3 , 5>  points_9;
    if(_S902)
    {
        float3  _S908 = s_primal_ctx_unproject_raydir_0(_S900._S870, camera_model_17, is_ray_depth_15);
        float3  _S909 = make_float3 (depths_7.x) * _S908;
        if(!_S900._S873)
        {
            _runFlag_15 = false;
        }
        else
        {
            _runFlag_15 = _S902;
        }
        if(_runFlag_15)
        {
            float3  _S910 = s_primal_ctx_unproject_raydir_0(_S900._S872, camera_model_17, is_ray_depth_15);
            float3  _S911 = make_float3 (depths_7.y) * _S910;
            if(!_S900._S875)
            {
                _runFlag_16 = false;
            }
            else
            {
                _runFlag_16 = _runFlag_15;
            }
            if(_runFlag_16)
            {
                float3  _S912 = s_primal_ctx_unproject_raydir_0(_S900._S874, camera_model_17, is_ray_depth_15);
                float3  _S913 = make_float3 (depths_7.z) * _S912;
                if(!_S900._S877)
                {
                    _runFlag_17 = false;
                }
                else
                {
                    _runFlag_17 = _runFlag_16;
                }
                if(_runFlag_17)
                {
                    float3  _S914 = s_primal_ctx_unproject_raydir_0(_S900._S876, camera_model_17, is_ray_depth_15);
                    float3  _S915 = make_float3 (depths_7.w) * _S914;
                    if(!_S900._S879)
                    {
                        _runFlag_18 = false;
                    }
                    else
                    {
                        _runFlag_18 = _runFlag_17;
                    }
                    if(_runFlag_18)
                    {
                        float3  _S916 = s_primal_ctx_unproject_raydir_0(_S900._S878, camera_model_17, is_ray_depth_15);
                        _S903 = int(1);
                        raydir_15 = _S916;
                    }
                    else
                    {
                        _S903 = int(0);
                        raydir_15 = _S914;
                    }
                    points_9[int(0)] = _S909;
                    points_9[int(1)] = _S911;
                    points_9[int(2)] = _S913;
                    points_9[int(3)] = _S915;
                    points_9[int(4)] = _S901;
                    _S904 = _S914;
                }
                else
                {
                    _S903 = int(0);
                    raydir_15 = _S912;
                    points_9[int(0)] = _S909;
                    points_9[int(1)] = _S911;
                    points_9[int(2)] = _S913;
                    points_9[int(3)] = _S901;
                    points_9[int(4)] = _S901;
                    _S904 = _S901;
                }
                _S905 = _S912;
            }
            else
            {
                _S903 = int(0);
                raydir_15 = _S910;
                points_9[int(0)] = _S909;
                points_9[int(1)] = _S911;
                points_9[int(2)] = _S901;
                points_9[int(3)] = _S901;
                points_9[int(4)] = _S901;
                _runFlag_17 = false;
                _S904 = _S901;
                _S905 = _S901;
            }
            _S906 = _S910;
        }
        else
        {
            _S903 = int(0);
            raydir_15 = _S908;
            points_9[int(0)] = _S909;
            points_9[int(1)] = _S901;
            points_9[int(2)] = _S901;
            points_9[int(3)] = _S901;
            points_9[int(4)] = _S901;
            _runFlag_16 = false;
            _runFlag_17 = false;
            _S904 = _S901;
            _S905 = _S901;
            _S906 = _S901;
        }
        _S907 = _S908;
    }
    else
    {
        _S903 = int(0);
        points_9[int(0)] = _S901;
        points_9[int(1)] = _S901;
        points_9[int(2)] = _S901;
        points_9[int(3)] = _S901;
        points_9[int(4)] = _S901;
        _runFlag_15 = false;
        _runFlag_16 = false;
        _runFlag_17 = false;
        _S904 = _S901;
        _S905 = _S901;
        _S906 = _S901;
        _S907 = _S901;
    }
    bool _S917 = !(_S903 != int(1));
    bool _S918;
    float3  normal_11;
    float3  _S919;
    float3  _S920;
    float3  _S921;
    float3  _S922;
    float _S923;
    float _S924;
    float _S925;
    float _S926;
    if(_S917)
    {
        float3  dx_4 = points_9[int(1)] - points_9[int(0)];
        float3  _S927 = - (points_9[int(3)] - points_9[int(2)]);
        float3  _S928 = s_primal_ctx_cross_0(dx_4, _S927);
        bool _S929 = (s_primal_ctx_dot_0(_S928, _S928)) != 0.0f;
        if(_S929)
        {
            normal_11 = normalize_0(_S928);
        }
        else
        {
            normal_11 = _S928;
        }
        bool _S930 = (s_primal_ctx_dot_0(gt_normal_3, gt_normal_3)) != 0.0f;
        if(_S930)
        {
            _S919 = normalize_0(gt_normal_3);
        }
        else
        {
            _S919 = gt_normal_3;
        }
        float3  _S931 = - normalize_0(raydir_15);
        float _S932 = s_primal_ctx_dot_0(normal_11, _S931);
        float _S933 = 1.0f - s_primal_ctx_dot_0(normal_11, _S919) + 0.00100000004749745f;
        float _S934 = (F32_max((_S932), (0.0f))) + 0.00100000004749745f;
        _S923 = _S934 * _S934;
        _S924 = _S933;
        _S925 = _S934;
        _S926 = _S932;
        raydir_15 = normal_11;
        normal_11 = _S931;
        _runFlag_18 = _S930;
        _S918 = _S929;
        _S920 = _S928;
        _S921 = dx_4;
        _S922 = _S927;
    }
    else
    {
        _S923 = 0.0f;
        _S924 = 0.0f;
        _S925 = 0.0f;
        _S926 = 0.0f;
        raydir_15 = _S901;
        normal_11 = _S901;
        _S919 = _S901;
        _runFlag_18 = false;
        _S918 = false;
        _S920 = _S901;
        _S921 = _S901;
        _S922 = _S901;
    }
    float4  _S935 = make_float4 (0.0f);
    if(_S917)
    {
        float _S936 = v_loss_1 / _S923;
        float _S937 = _S924 * - _S936;
        float s_diff_num_T_1 = _S925 * _S936;
        DiffPair_float_0 _S938;
        (&_S938)->primal_0 = _S926;
        (&_S938)->differential_0 = 0.0f;
        DiffPair_float_0 _S939;
        (&_S939)->primal_0 = 0.0f;
        (&_S939)->differential_0 = 0.0f;
        _d_max_0(&_S938, &_S939, _S937);
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S940;
        (&_S940)->primal_0 = raydir_15;
        (&_S940)->differential_0 = _S901;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S941;
        (&_S941)->primal_0 = normal_11;
        (&_S941)->differential_0 = _S901;
        s_bwd_prop_dot_0(&_S940, &_S941, _S938.differential_0);
        float _S942 = - s_diff_num_T_1;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S943;
        (&_S943)->primal_0 = raydir_15;
        (&_S943)->differential_0 = _S901;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S944;
        (&_S944)->primal_0 = _S919;
        (&_S944)->differential_0 = _S901;
        s_bwd_prop_dot_0(&_S943, &_S944, _S942);
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S945 = _S944;
        float3  _S946 = _S940.differential_0 + _S943.differential_0;
        if(_runFlag_18)
        {
            DiffPair_vectorx3Cfloatx2C3x3E_0 _S947;
            (&_S947)->primal_0 = gt_normal_3;
            (&_S947)->differential_0 = _S901;
            s_bwd_normalize_impl_0(&_S947, _S945.differential_0);
            raydir_15 = _S947.differential_0;
        }
        else
        {
            raydir_15 = _S945.differential_0;
        }
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S948;
        (&_S948)->primal_0 = gt_normal_3;
        (&_S948)->differential_0 = _S901;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S949;
        (&_S949)->primal_0 = gt_normal_3;
        (&_S949)->differential_0 = _S901;
        s_bwd_prop_dot_0(&_S948, &_S949, 0.0f);
        float3  _S950 = _S949.differential_0 + _S948.differential_0 + raydir_15;
        if(_S918)
        {
            DiffPair_vectorx3Cfloatx2C3x3E_0 _S951;
            (&_S951)->primal_0 = _S920;
            (&_S951)->differential_0 = _S901;
            s_bwd_normalize_impl_0(&_S951, _S946);
            raydir_15 = _S951.differential_0;
        }
        else
        {
            raydir_15 = _S946;
        }
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S952;
        (&_S952)->primal_0 = _S920;
        (&_S952)->differential_0 = _S901;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S953;
        (&_S953)->primal_0 = _S920;
        (&_S953)->differential_0 = _S901;
        s_bwd_prop_dot_0(&_S952, &_S953, 0.0f);
        float3  _S954 = _S953.differential_0 + _S952.differential_0 + raydir_15;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S955;
        (&_S955)->primal_0 = _S921;
        (&_S955)->differential_0 = _S901;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S956;
        (&_S956)->primal_0 = _S922;
        (&_S956)->differential_0 = _S901;
        s_bwd_prop_cross_0(&_S955, &_S956, _S954);
        float3  s_diff_dy_T_4 = - _S956.differential_0;
        float3  _S957 = - s_diff_dy_T_4;
        float3  _S958 = - _S955.differential_0;
        FixedArray<float3 , 5>  _S959;
        _S959[int(0)] = _S901;
        _S959[int(1)] = _S901;
        _S959[int(2)] = _S901;
        _S959[int(3)] = _S901;
        _S959[int(4)] = _S901;
        _S959[int(2)] = _S957;
        _S959[int(3)] = s_diff_dy_T_4;
        _S959[int(0)] = _S958;
        _S959[int(1)] = _S955.differential_0;
        points_9[int(0)] = _S959[int(0)];
        points_9[int(1)] = _S959[int(1)];
        points_9[int(2)] = _S959[int(2)];
        points_9[int(3)] = _S959[int(3)];
        points_9[int(4)] = _S959[int(4)];
        raydir_15 = _S950;
    }
    else
    {
        points_9[int(0)] = _S901;
        points_9[int(1)] = _S901;
        points_9[int(2)] = _S901;
        points_9[int(3)] = _S901;
        points_9[int(4)] = _S901;
        raydir_15 = _S901;
    }
    float4  _S960;
    if(_S902)
    {
        if(_runFlag_15)
        {
            if(_runFlag_16)
            {
                if(_runFlag_17)
                {
                    FixedArray<float3 , 5>  _S961 = points_9;
                    FixedArray<float3 , 5>  _S962 = points_9;
                    FixedArray<float3 , 5>  _S963 = points_9;
                    float3  _S964 = _S904 * points_9[int(3)];
                    float _S965 = _S964.x + _S964.y + _S964.z;
                    float4  _S966 = _S935;
                    *&((&_S966)->w) = _S965;
                    points_9[int(0)] = _S901;
                    points_9[int(1)] = _S901;
                    points_9[int(2)] = _S901;
                    points_9[int(3)] = _S901;
                    points_9[int(4)] = _S901;
                    _S904 = _S963[int(2)];
                    normal_11 = _S961[int(0)];
                    _S919 = _S962[int(1)];
                    _S960 = _S966;
                }
                else
                {
                    FixedArray<float3 , 5>  _S967 = points_9;
                    FixedArray<float3 , 5>  _S968 = points_9;
                    FixedArray<float3 , 5>  _S969 = points_9;
                    FixedArray<float3 , 5>  _S970 = points_9;
                    points_9[int(0)] = points_9[int(0)];
                    points_9[int(1)] = _S967[int(1)];
                    points_9[int(2)] = _S968[int(2)];
                    points_9[int(3)] = _S969[int(3)];
                    points_9[int(4)] = _S970[int(4)];
                    _S904 = _S901;
                    normal_11 = _S901;
                    _S919 = _S901;
                    _S960 = _S935;
                }
                float3  _S971 = _S905 * (points_9[int(2)] + _S904);
                float _S972 = _S971.x + _S971.y + _S971.z;
                float3  _S973 = points_9[int(0)] + normal_11;
                float3  _S974 = points_9[int(1)] + _S919;
                float4  _S975 = _S935;
                *&((&_S975)->z) = _S972;
                float4  _S976 = _S960 + _S975;
                points_9[int(0)] = _S901;
                points_9[int(1)] = _S901;
                points_9[int(2)] = _S901;
                points_9[int(3)] = _S901;
                points_9[int(4)] = _S901;
                _S904 = _S974;
                _S905 = _S973;
                _S960 = _S976;
            }
            else
            {
                FixedArray<float3 , 5>  _S977 = points_9;
                FixedArray<float3 , 5>  _S978 = points_9;
                FixedArray<float3 , 5>  _S979 = points_9;
                FixedArray<float3 , 5>  _S980 = points_9;
                points_9[int(0)] = points_9[int(0)];
                points_9[int(1)] = _S977[int(1)];
                points_9[int(2)] = _S978[int(2)];
                points_9[int(3)] = _S979[int(3)];
                points_9[int(4)] = _S980[int(4)];
                _S904 = _S901;
                _S905 = _S901;
                _S960 = _S935;
            }
            float3  _S981 = _S906 * (points_9[int(1)] + _S904);
            float _S982 = _S981.x + _S981.y + _S981.z;
            float3  _S983 = points_9[int(0)] + _S905;
            float4  _S984 = _S935;
            *&((&_S984)->y) = _S982;
            float4  _S985 = _S960 + _S984;
            points_9[int(0)] = _S901;
            points_9[int(1)] = _S901;
            points_9[int(2)] = _S901;
            points_9[int(3)] = _S901;
            points_9[int(4)] = _S901;
            _S904 = _S983;
            _S960 = _S985;
        }
        else
        {
            FixedArray<float3 , 5>  _S986 = points_9;
            FixedArray<float3 , 5>  _S987 = points_9;
            FixedArray<float3 , 5>  _S988 = points_9;
            FixedArray<float3 , 5>  _S989 = points_9;
            points_9[int(0)] = points_9[int(0)];
            points_9[int(1)] = _S986[int(1)];
            points_9[int(2)] = _S987[int(2)];
            points_9[int(3)] = _S988[int(3)];
            points_9[int(4)] = _S989[int(4)];
            _S904 = _S901;
            _S960 = _S935;
        }
        float3  _S990 = _S907 * (points_9[int(0)] + _S904);
        float _S991 = _S990.x + _S990.y + _S990.z;
        float4  _S992 = _S935;
        *&((&_S992)->x) = _S991;
        _S960 = _S960 + _S992;
    }
    else
    {
        _S960 = _S935;
    }
    *v_depths_3 = _S960;
    *v_gt_normal_1 = raydir_15;
    return;
}

inline __device__ float3  generate_ray_d2n_prism(float2  pix_pos_6, float4  intrins_16, FixedArray<float, 8>  dist_coeffs_19, int camera_model_18, bool is_ray_depth_16)
{
    float3  _S993;
    for(;;)
    {
        float2  uv_50 = (pix_pos_6 - float2 {intrins_16.z, intrins_16.w}) / float2 {intrins_16.x, intrins_16.y};
        FixedArray<float, 8>  _S994 = dist_coeffs_19;
        float2  uv_u_24;
        bool _S995 = undistort_point_2(uv_50, &_S994, int(12), &uv_u_24);
        if(!_S995)
        {
            int3  _S996 = make_int3 (int(0));
            float3  _S997 = make_float3 ((float)_S996.x, (float)_S996.y, (float)_S996.z);
            _S993 = _S997;
            break;
        }
        _S993 = unproject_raydir_0(uv_u_24, camera_model_18, is_ray_depth_16);
        break;
    }
    return _S993;
}

inline __device__ float3  depth_to_point_prism(float2  pix_pos_7, float4  intrins_17, FixedArray<float, 8>  dist_coeffs_20, int camera_model_19, bool is_ray_depth_17, float depth_6)
{
    float3  _S998;
    for(;;)
    {
        float2  uv_51 = (pix_pos_7 - float2 {intrins_17.z, intrins_17.w}) / float2 {intrins_17.x, intrins_17.y};
        FixedArray<float, 8>  _S999 = dist_coeffs_20;
        float2  uv_u_25;
        bool _S1000 = undistort_point_2(uv_51, &_S999, int(12), &uv_u_25);
        if(!_S1000)
        {
            _S998 = make_float3 (0.0f);
            break;
        }
        _S998 = make_float3 (depth_6) * unproject_raydir_0(uv_u_25, camera_model_19, is_ray_depth_17);
        break;
    }
    return _S998;
}

struct s_bwd_prop_depth_to_point_Intermediates_2
{
    float2  _S1001;
    bool _S1002;
};

inline __device__ float depth_to_point_vjp_prism(float2  pix_pos_8, float4  intrins_18, FixedArray<float, 8>  dist_coeffs_21, int camera_model_20, bool is_ray_depth_18, float depth_7, float3  v_point_2)
{
    float2  _S1003 = make_float2 (0.0f);
    s_bwd_prop_depth_to_point_Intermediates_2 _S1004;
    (&_S1004)->_S1001 = _S1003;
    (&_S1004)->_S1002 = false;
    float2  uv_52 = (pix_pos_8 - float2 {intrins_18.z, intrins_18.w}) / float2 {intrins_18.x, intrins_18.y};
    float2  _S1005 = _S1003;
    FixedArray<float, 8>  _S1006 = dist_coeffs_21;
    bool _S1007 = undistort_point_2(uv_52, &_S1006, int(12), &_S1005);
    (&_S1004)->_S1001 = _S1005;
    (&_S1004)->_S1002 = _S1007;
    s_bwd_prop_depth_to_point_Intermediates_2 _S1008 = _S1004;
    float3  _S1009 = make_float3 (0.0f);
    bool _S1010 = !!_S1004._S1002;
    float3  _S1011;
    if(_S1010)
    {
        _S1011 = s_primal_ctx_unproject_raydir_0(_S1008._S1001, camera_model_20, is_ray_depth_18);
    }
    else
    {
        _S1011 = _S1009;
    }
    if(_S1010)
    {
        _S1011 = _S1011 * v_point_2;
    }
    else
    {
        _S1011 = _S1009;
    }
    return _S1011.x + _S1011.y + _S1011.z;
}

inline __device__ float3  depth_to_normal_prism(float2  pix_center_10, float4  intrins_19, FixedArray<float, 8>  dist_coeffs_22, int camera_model_21, bool is_ray_depth_19, float4  depths_8)
{
    float3  normal_12;
    for(;;)
    {
        bool _S1012;
        if((depths_8.x) == 0.0f)
        {
            _S1012 = true;
        }
        else
        {
            _S1012 = (depths_8.y) == 0.0f;
        }
        if(_S1012)
        {
            _S1012 = true;
        }
        else
        {
            _S1012 = (depths_8.z) == 0.0f;
        }
        if(_S1012)
        {
            _S1012 = true;
        }
        else
        {
            _S1012 = (depths_8.w) == 0.0f;
        }
        if(_S1012)
        {
            normal_12 = make_float3 (0.0f);
            break;
        }
        float3  * _S1013;
        float3  * _S1014;
        float3  * _S1015;
        float3  * _S1016;
        int _S1017;
        FixedArray<float3 , 4>  points_10;
        for(;;)
        {
            float2  _S1018 = float2 {intrins_19.z, intrins_19.w};
            float2  _S1019 = float2 {intrins_19.x, intrins_19.y};
            float2  uv_53 = (pix_center_10 + make_float2 (-1.0f, -0.0f) - _S1018) / _S1019;
            FixedArray<float, 8>  _S1020 = dist_coeffs_22;
            float2  uv_u_26;
            bool _S1021 = undistort_point_2(uv_53, &_S1020, int(12), &uv_u_26);
            if(!_S1021)
            {
                float3  _S1022 = make_float3 (0.0f);
                _S1017 = int(0);
                _S1016 = nullptr;
                _S1015 = nullptr;
                _S1014 = nullptr;
                _S1013 = nullptr;
                normal_12 = _S1022;
                break;
            }
            points_10[int(0)] = make_float3 (depths_8.x) * unproject_raydir_0(uv_u_26, camera_model_21, is_ray_depth_19);
            for(;;)
            {
                float2  uv_54 = (pix_center_10 + make_float2 (1.0f, -0.0f) - _S1018) / _S1019;
                FixedArray<float, 8>  _S1023 = dist_coeffs_22;
                float2  uv_u_27;
                bool _S1024 = undistort_point_2(uv_54, &_S1023, int(12), &uv_u_27);
                if(!_S1024)
                {
                    float3  _S1025 = make_float3 (0.0f);
                    _S1017 = int(0);
                    _S1016 = nullptr;
                    normal_12 = _S1025;
                    break;
                }
                points_10[int(1)] = make_float3 (depths_8.y) * unproject_raydir_0(uv_u_27, camera_model_21, is_ray_depth_19);
                _S1017 = int(2);
                _S1016 = &points_10[int(1)];
                break;
            }
            if(_S1017 != int(2))
            {
                _S1015 = &points_10[int(0)];
                _S1014 = nullptr;
                _S1013 = nullptr;
                break;
            }
            float2  uv_55 = (pix_center_10 + make_float2 (0.0f, -1.0f) - _S1018) / _S1019;
            FixedArray<float, 8>  _S1026 = dist_coeffs_22;
            float2  uv_u_28;
            bool _S1027 = undistort_point_2(uv_55, &_S1026, int(12), &uv_u_28);
            if(!_S1027)
            {
                float3  _S1028 = make_float3 (0.0f);
                _S1017 = int(0);
                _S1015 = &points_10[int(0)];
                _S1014 = nullptr;
                _S1013 = nullptr;
                normal_12 = _S1028;
                break;
            }
            points_10[int(2)] = make_float3 (depths_8.z) * unproject_raydir_0(uv_u_28, camera_model_21, is_ray_depth_19);
            for(;;)
            {
                float2  uv_56 = (pix_center_10 + make_float2 (0.0f, 1.0f) - _S1018) / _S1019;
                FixedArray<float, 8>  _S1029 = dist_coeffs_22;
                float2  uv_u_29;
                bool _S1030 = undistort_point_2(uv_56, &_S1029, int(12), &uv_u_29);
                if(!_S1030)
                {
                    float3  _S1031 = make_float3 (0.0f);
                    _S1017 = int(0);
                    _S1015 = nullptr;
                    normal_12 = _S1031;
                    break;
                }
                points_10[int(3)] = make_float3 (depths_8.w) * unproject_raydir_0(uv_u_29, camera_model_21, is_ray_depth_19);
                _S1017 = int(2);
                _S1015 = &points_10[int(3)];
                break;
            }
            if(_S1017 != int(2))
            {
                float3  * _S1032 = _S1015;
                _S1015 = &points_10[int(0)];
                _S1014 = _S1032;
                _S1013 = &points_10[int(2)];
                break;
            }
            float3  * _S1033 = _S1015;
            _S1017 = int(1);
            _S1015 = &points_10[int(0)];
            _S1014 = _S1033;
            _S1013 = &points_10[int(2)];
            break;
        }
        if(_S1017 != int(1))
        {
            break;
        }
        float3  normal_13 = cross_0(*_S1016 - *_S1015, - (*_S1014 - *_S1013));
        if((dot_0(normal_13, normal_13)) != 0.0f)
        {
            normal_12 = normal_13 / make_float3 (length_0(normal_13));
        }
        else
        {
            normal_12 = normal_13;
        }
        break;
    }
    return normal_12;
}

struct s_bwd_prop_depth_to_normal_Intermediates_2
{
    float2  _S1034;
    bool _S1035;
    float2  _S1036;
    bool _S1037;
    float2  _S1038;
    bool _S1039;
    float2  _S1040;
    bool _S1041;
};

inline __device__ void depth_to_normal_vjp_prism(float2  pix_center_11, float4  intrins_20, FixedArray<float, 8>  dist_coeffs_23, int camera_model_22, bool is_ray_depth_20, float4  depths_9, float3  v_normal_3, float4  * v_depths_4)
{
    float2  _S1042 = make_float2 (0.0f);
    s_bwd_prop_depth_to_normal_Intermediates_2 _S1043;
    (&_S1043)->_S1034 = _S1042;
    (&_S1043)->_S1035 = false;
    (&_S1043)->_S1036 = _S1042;
    (&_S1043)->_S1037 = false;
    (&_S1043)->_S1038 = _S1042;
    (&_S1043)->_S1039 = false;
    (&_S1043)->_S1040 = _S1042;
    (&_S1043)->_S1041 = false;
    (&_S1043)->_S1034 = _S1042;
    (&_S1043)->_S1035 = false;
    (&_S1043)->_S1036 = _S1042;
    (&_S1043)->_S1037 = false;
    (&_S1043)->_S1038 = _S1042;
    (&_S1043)->_S1039 = false;
    (&_S1043)->_S1040 = _S1042;
    (&_S1043)->_S1041 = false;
    bool _S1044 = (depths_9.x) == 0.0f;
    bool _runFlag_19;
    if(_S1044)
    {
        _runFlag_19 = true;
    }
    else
    {
        _runFlag_19 = (depths_9.y) == 0.0f;
    }
    if(_runFlag_19)
    {
        _runFlag_19 = true;
    }
    else
    {
        _runFlag_19 = (depths_9.z) == 0.0f;
    }
    if(_runFlag_19)
    {
        _runFlag_19 = true;
    }
    else
    {
        _runFlag_19 = (depths_9.w) == 0.0f;
    }
    int _S1045;
    if(!_runFlag_19)
    {
        float2  _S1046 = float2 {intrins_20.z, intrins_20.w};
        float2  _S1047 = float2 {intrins_20.x, intrins_20.y};
        float2  uv_57 = (pix_center_11 + make_float2 (-1.0f, -0.0f) - _S1046) / _S1047;
        float2  _S1048 = _S1042;
        FixedArray<float, 8>  _S1049 = dist_coeffs_23;
        bool _S1050 = undistort_point_2(uv_57, &_S1049, int(12), &_S1048);
        (&_S1043)->_S1034 = _S1048;
        (&_S1043)->_S1035 = _S1050;
        bool _S1051 = !!_S1050;
        if(_S1051)
        {
            float2  uv_58 = (pix_center_11 + make_float2 (1.0f, -0.0f) - _S1046) / _S1047;
            float2  _S1052 = _S1042;
            FixedArray<float, 8>  _S1053 = dist_coeffs_23;
            bool _S1054 = undistort_point_2(uv_58, &_S1053, int(12), &_S1052);
            (&_S1043)->_S1036 = _S1052;
            (&_S1043)->_S1037 = _S1054;
            if(!!_S1054)
            {
                _S1045 = int(2);
            }
            else
            {
                _S1045 = int(0);
            }
            if(_S1045 != int(2))
            {
                _runFlag_19 = false;
            }
            else
            {
                _runFlag_19 = _S1051;
            }
            if(_runFlag_19)
            {
                float2  uv_59 = (pix_center_11 + make_float2 (0.0f, -1.0f) - _S1046) / _S1047;
                float2  _S1055 = _S1042;
                FixedArray<float, 8>  _S1056 = dist_coeffs_23;
                bool _S1057 = undistort_point_2(uv_59, &_S1056, int(12), &_S1055);
                (&_S1043)->_S1038 = _S1055;
                (&_S1043)->_S1039 = _S1057;
                if(!_S1057)
                {
                    _runFlag_19 = false;
                }
                if(_runFlag_19)
                {
                    float2  uv_60 = (pix_center_11 + make_float2 (0.0f, 1.0f) - _S1046) / _S1047;
                    float2  _S1058 = _S1042;
                    FixedArray<float, 8>  _S1059 = dist_coeffs_23;
                    bool _S1060 = undistort_point_2(uv_60, &_S1059, int(12), &_S1058);
                    (&_S1043)->_S1040 = _S1058;
                    (&_S1043)->_S1041 = _S1060;
                }
            }
        }
    }
    s_bwd_prop_depth_to_normal_Intermediates_2 _S1061 = _S1043;
    float3  _S1062 = make_float3 (0.0f);
    if(_S1044)
    {
        _runFlag_19 = true;
    }
    else
    {
        _runFlag_19 = (depths_9.y) == 0.0f;
    }
    if(_runFlag_19)
    {
        _runFlag_19 = true;
    }
    else
    {
        _runFlag_19 = (depths_9.z) == 0.0f;
    }
    if(_runFlag_19)
    {
        _runFlag_19 = true;
    }
    else
    {
        _runFlag_19 = (depths_9.w) == 0.0f;
    }
    bool _S1063 = !_runFlag_19;
    bool _runFlag_20;
    bool _runFlag_21;
    bool _S1064;
    bool _runFlag_22;
    bool _S1065;
    bool _S1066;
    FixedArray<float3 , 4>  points_11;
    float3  _S1067;
    float3  _S1068;
    float3  _S1069;
    float3  _S1070;
    float3  _S1071;
    float3  _S1072;
    float3  _S1073;
    float3  _S1074;
    float3  _S1075;
    if(_S1063)
    {
        bool _S1076 = !!_S1061._S1035;
        if(_S1076)
        {
            float3  _S1077 = s_primal_ctx_unproject_raydir_0(_S1061._S1034, camera_model_22, is_ray_depth_20);
            float3  _S1078 = make_float3 (depths_9.x) * _S1077;
            bool _S1079 = !!_S1061._S1037;
            if(_S1079)
            {
                float3  _S1080 = s_primal_ctx_unproject_raydir_0(_S1061._S1036, camera_model_22, is_ray_depth_20);
                float3  _S1081 = make_float3 (depths_9.y) * _S1080;
                _S1045 = int(2);
                points_11[int(0)] = _S1078;
                points_11[int(1)] = _S1081;
                points_11[int(2)] = _S1062;
                points_11[int(3)] = _S1062;
                _S1067 = _S1080;
            }
            else
            {
                _S1045 = int(0);
                points_11[int(0)] = _S1078;
                points_11[int(1)] = _S1062;
                points_11[int(2)] = _S1062;
                points_11[int(3)] = _S1062;
                _S1067 = _S1062;
            }
            if(_S1045 != int(2))
            {
                _runFlag_19 = false;
            }
            else
            {
                _runFlag_19 = _S1076;
                _S1045 = int(0);
            }
            if(_runFlag_19)
            {
                if(!_S1061._S1039)
                {
                    _runFlag_20 = false;
                    _S1045 = int(0);
                }
                else
                {
                    _runFlag_20 = _runFlag_19;
                }
                if(_runFlag_20)
                {
                    float3  _S1082 = s_primal_ctx_unproject_raydir_0(_S1061._S1038, camera_model_22, is_ray_depth_20);
                    points_11[int(2)] = make_float3 (depths_9.z) * _S1082;
                    bool _S1083 = !!_S1061._S1041;
                    int _S1084;
                    if(_S1083)
                    {
                        float3  _S1085 = s_primal_ctx_unproject_raydir_0(_S1061._S1040, camera_model_22, is_ray_depth_20);
                        points_11[int(3)] = make_float3 (depths_9.w) * _S1085;
                        _S1084 = int(2);
                        _S1068 = _S1085;
                    }
                    else
                    {
                        _S1084 = int(0);
                        _S1068 = _S1062;
                    }
                    if(_S1084 != int(2))
                    {
                        _runFlag_21 = false;
                        _S1045 = _S1084;
                    }
                    else
                    {
                        _runFlag_21 = _runFlag_20;
                    }
                    if(_runFlag_21)
                    {
                        _S1045 = int(1);
                    }
                    _runFlag_21 = _S1083;
                    _S1069 = _S1082;
                }
                else
                {
                    _runFlag_21 = false;
                    _S1068 = _S1062;
                    _S1069 = _S1062;
                }
            }
            else
            {
                _runFlag_20 = false;
                _runFlag_21 = false;
                _S1068 = _S1062;
                _S1069 = _S1062;
            }
            float3  _S1086 = _S1067;
            _S1067 = _S1068;
            _S1068 = _S1069;
            _S1064 = _S1079;
            _S1069 = _S1086;
            _S1070 = _S1077;
        }
        else
        {
            _S1045 = int(0);
            points_11[int(0)] = _S1062;
            points_11[int(1)] = _S1062;
            points_11[int(2)] = _S1062;
            points_11[int(3)] = _S1062;
            _runFlag_19 = false;
            _runFlag_20 = false;
            _runFlag_21 = false;
            _S1067 = _S1062;
            _S1068 = _S1062;
            _S1064 = false;
            _S1069 = _S1062;
            _S1070 = _S1062;
        }
        if(_S1045 != int(1))
        {
            _runFlag_22 = false;
        }
        else
        {
            _runFlag_22 = _S1063;
        }
        if(_runFlag_22)
        {
            float3  dx_5 = points_11[int(1)] - points_11[int(0)];
            float3  _S1087 = - (points_11[int(3)] - points_11[int(2)]);
            float3  _S1088 = s_primal_ctx_cross_0(dx_5, _S1087);
            bool _S1089 = (s_primal_ctx_dot_0(_S1088, _S1088)) != 0.0f;
            if(_S1089)
            {
                float _S1090 = length_0(_S1088);
                float3  _S1091 = make_float3 (_S1090);
                _S1071 = make_float3 (_S1090 * _S1090);
                _S1072 = _S1091;
            }
            else
            {
                _S1071 = _S1062;
                _S1072 = _S1062;
            }
            float3  _S1092 = _S1072;
            _S1065 = _S1089;
            _S1072 = _S1088;
            _S1073 = _S1092;
            _S1074 = dx_5;
            _S1075 = _S1087;
        }
        else
        {
            _S1065 = false;
            _S1071 = _S1062;
            _S1072 = _S1062;
            _S1073 = _S1062;
            _S1074 = _S1062;
            _S1075 = _S1062;
        }
        bool _S1093 = _runFlag_19;
        bool _S1094 = _runFlag_20;
        bool _S1095 = _runFlag_21;
        float3  _S1096 = _S1067;
        float3  _S1097 = _S1068;
        bool _S1098 = _S1064;
        float3  _S1099 = _S1069;
        float3  _S1100 = _S1070;
        _runFlag_19 = _runFlag_22;
        _runFlag_20 = _S1065;
        _S1067 = _S1071;
        _S1068 = _S1072;
        _S1069 = _S1073;
        _S1070 = _S1074;
        _S1071 = _S1075;
        _runFlag_21 = _S1076;
        _S1064 = _S1093;
        _runFlag_22 = _S1094;
        _S1065 = _S1095;
        _S1072 = _S1096;
        _S1073 = _S1097;
        _S1066 = _S1098;
        _S1074 = _S1099;
        _S1075 = _S1100;
    }
    else
    {
        _runFlag_19 = false;
        _runFlag_20 = false;
        _S1067 = _S1062;
        _S1068 = _S1062;
        _S1069 = _S1062;
        _S1070 = _S1062;
        _S1071 = _S1062;
        _runFlag_21 = false;
        _S1064 = false;
        _runFlag_22 = false;
        _S1065 = false;
        _S1072 = _S1062;
        _S1073 = _S1062;
        _S1066 = false;
        _S1074 = _S1062;
        _S1075 = _S1062;
    }
    float4  _S1101 = make_float4 (0.0f);
    float4  _S1102;
    if(_S1063)
    {
        if(_runFlag_19)
        {
            if(_runFlag_20)
            {
                float3  _S1103 = v_normal_3 / _S1067;
                float3  _S1104 = _S1068 * - _S1103;
                float3  _S1105 = _S1069 * _S1103;
                float _S1106 = _S1104.x + _S1104.y + _S1104.z;
                DiffPair_vectorx3Cfloatx2C3x3E_0 _S1107;
                (&_S1107)->primal_0 = _S1068;
                (&_S1107)->differential_0 = _S1062;
                s_bwd_length_impl_0(&_S1107, _S1106);
                _S1067 = _S1105 + _S1107.differential_0;
            }
            else
            {
                _S1067 = v_normal_3;
            }
            DiffPair_vectorx3Cfloatx2C3x3E_0 _S1108;
            (&_S1108)->primal_0 = _S1068;
            (&_S1108)->differential_0 = _S1062;
            DiffPair_vectorx3Cfloatx2C3x3E_0 _S1109;
            (&_S1109)->primal_0 = _S1068;
            (&_S1109)->differential_0 = _S1062;
            s_bwd_prop_dot_0(&_S1108, &_S1109, 0.0f);
            float3  _S1110 = _S1109.differential_0 + _S1108.differential_0 + _S1067;
            DiffPair_vectorx3Cfloatx2C3x3E_0 _S1111;
            (&_S1111)->primal_0 = _S1070;
            (&_S1111)->differential_0 = _S1062;
            DiffPair_vectorx3Cfloatx2C3x3E_0 _S1112;
            (&_S1112)->primal_0 = _S1071;
            (&_S1112)->differential_0 = _S1062;
            s_bwd_prop_cross_0(&_S1111, &_S1112, _S1110);
            float3  s_diff_dy_T_5 = - _S1112.differential_0;
            float3  _S1113 = - s_diff_dy_T_5;
            float3  _S1114 = - _S1111.differential_0;
            FixedArray<float3 , 4>  _S1115;
            _S1115[int(0)] = _S1062;
            _S1115[int(1)] = _S1062;
            _S1115[int(2)] = _S1062;
            _S1115[int(3)] = _S1062;
            _S1115[int(2)] = _S1113;
            _S1115[int(3)] = s_diff_dy_T_5;
            _S1115[int(0)] = _S1114;
            _S1115[int(1)] = _S1111.differential_0;
            points_11[int(0)] = _S1115[int(0)];
            points_11[int(1)] = _S1115[int(1)];
            points_11[int(2)] = _S1115[int(2)];
            points_11[int(3)] = _S1115[int(3)];
        }
        else
        {
            points_11[int(0)] = _S1062;
            points_11[int(1)] = _S1062;
            points_11[int(2)] = _S1062;
            points_11[int(3)] = _S1062;
        }
        if(_runFlag_21)
        {
            if(_S1064)
            {
                if(_runFlag_22)
                {
                    FixedArray<float3 , 4>  _S1116 = points_11;
                    FixedArray<float3 , 4>  _S1117 = points_11;
                    FixedArray<float3 , 4>  _S1118 = points_11;
                    FixedArray<float3 , 4>  _S1119 = points_11;
                    if(_S1065)
                    {
                        float3  _S1120 = _S1072 * _S1119[int(3)];
                        float _S1121 = _S1120.x + _S1120.y + _S1120.z;
                        float4  _S1122 = _S1101;
                        *&((&_S1122)->w) = _S1121;
                        points_11[int(0)] = _S1116[int(0)];
                        points_11[int(1)] = _S1117[int(1)];
                        points_11[int(2)] = _S1118[int(2)];
                        points_11[int(3)] = _S1062;
                        _S1102 = _S1122;
                    }
                    else
                    {
                        points_11[int(0)] = _S1116[int(0)];
                        points_11[int(1)] = _S1117[int(1)];
                        points_11[int(2)] = _S1118[int(2)];
                        points_11[int(3)] = _S1119[int(3)];
                        _S1102 = _S1101;
                    }
                    float3  _S1123 = _S1073 * points_11[int(2)];
                    float _S1124 = _S1123.x + _S1123.y + _S1123.z;
                    FixedArray<float3 , 4>  _S1125 = points_11;
                    FixedArray<float3 , 4>  _S1126 = points_11;
                    float4  _S1127 = _S1101;
                    *&((&_S1127)->z) = _S1124;
                    float4  _S1128 = _S1102 + _S1127;
                    points_11[int(0)] = points_11[int(0)];
                    points_11[int(1)] = _S1125[int(1)];
                    points_11[int(2)] = _S1062;
                    points_11[int(3)] = _S1126[int(3)];
                    _S1102 = _S1128;
                }
                else
                {
                    FixedArray<float3 , 4>  _S1129 = points_11;
                    FixedArray<float3 , 4>  _S1130 = points_11;
                    FixedArray<float3 , 4>  _S1131 = points_11;
                    points_11[int(0)] = points_11[int(0)];
                    points_11[int(1)] = _S1129[int(1)];
                    points_11[int(2)] = _S1130[int(2)];
                    points_11[int(3)] = _S1131[int(3)];
                    _S1102 = _S1101;
                }
            }
            else
            {
                FixedArray<float3 , 4>  _S1132 = points_11;
                FixedArray<float3 , 4>  _S1133 = points_11;
                FixedArray<float3 , 4>  _S1134 = points_11;
                points_11[int(0)] = points_11[int(0)];
                points_11[int(1)] = _S1132[int(1)];
                points_11[int(2)] = _S1133[int(2)];
                points_11[int(3)] = _S1134[int(3)];
                _S1102 = _S1101;
            }
            if(_S1066)
            {
                FixedArray<float3 , 4>  _S1135 = points_11;
                float3  _S1136 = _S1074 * points_11[int(1)];
                float _S1137 = _S1136.x + _S1136.y + _S1136.z;
                float4  _S1138 = _S1101;
                *&((&_S1138)->y) = _S1137;
                float4  _S1139 = _S1102 + _S1138;
                points_11[int(0)] = _S1062;
                points_11[int(1)] = _S1062;
                points_11[int(2)] = _S1062;
                points_11[int(3)] = _S1062;
                _S1067 = _S1135[int(0)];
                _S1102 = _S1139;
            }
            else
            {
                FixedArray<float3 , 4>  _S1140 = points_11;
                FixedArray<float3 , 4>  _S1141 = points_11;
                FixedArray<float3 , 4>  _S1142 = points_11;
                points_11[int(0)] = points_11[int(0)];
                points_11[int(1)] = _S1140[int(1)];
                points_11[int(2)] = _S1141[int(2)];
                points_11[int(3)] = _S1142[int(3)];
                _S1067 = _S1062;
            }
            float3  _S1143 = _S1075 * (points_11[int(0)] + _S1067);
            float _S1144 = _S1143.x + _S1143.y + _S1143.z;
            float4  _S1145 = _S1101;
            *&((&_S1145)->x) = _S1144;
            _S1102 = _S1102 + _S1145;
        }
        else
        {
            _S1102 = _S1101;
        }
    }
    else
    {
        _S1102 = _S1101;
    }
    *v_depths_4 = _S1102;
    return;
}

inline __device__ float ray_depth_to_linear_depth_factor_prism(float2  pix_center_12, float4  intrins_21, FixedArray<float, 8>  dist_coeffs_24, int camera_model_23)
{
    float _S1146;
    for(;;)
    {
        float2  uv_61 = (pix_center_12 - float2 {intrins_21.z, intrins_21.w}) / float2 {intrins_21.x, intrins_21.y};
        FixedArray<float, 8>  _S1147 = dist_coeffs_24;
        float2  uv_u_30;
        bool _S1148 = undistort_point_2(uv_61, &_S1147, int(12), &uv_u_30);
        if(!_S1148)
        {
            _S1146 = 0.0f;
            break;
        }
        float3  raydir_16 = unproject_raydir_0(uv_u_30, camera_model_23, false);
        _S1146 = float((F32_sign((raydir_16.z)))) / length_0(raydir_16);
        break;
    }
    return _S1146;
}

inline __device__ float depth_normal_loss_prism(float2  pix_center_13, float4  intrins_22, FixedArray<float, 8>  dist_coeffs_25, int camera_model_24, bool is_ray_depth_21, float4  depths_10, float3  gt_normal_4)
{
    float _S1149;
    for(;;)
    {
        float3  _S1150;
        float3  * _S1151;
        float3  * _S1152;
        float3  * _S1153;
        float3  * _S1154;
        int _S1155;
        FixedArray<float3 , 5>  points_12;
        for(;;)
        {
            float2  _S1156 = float2 {intrins_22.z, intrins_22.w};
            float2  _S1157 = float2 {intrins_22.x, intrins_22.y};
            float2  uv_62 = (pix_center_13 + make_float2 (-1.0f, -0.0f) - _S1156) / _S1157;
            FixedArray<float, 8>  _S1158 = dist_coeffs_25;
            float2  uv_u_31;
            bool _S1159 = undistort_point_2(uv_62, &_S1158, int(12), &uv_u_31);
            float3  _S1160 = make_float3 (0.0f);
            if(!_S1159)
            {
                _S1155 = int(0);
                _S1154 = nullptr;
                _S1153 = nullptr;
                _S1152 = nullptr;
                _S1151 = nullptr;
                _S1150 = _S1160;
                break;
            }
            float3  raydir_17 = unproject_raydir_0(uv_u_31, camera_model_24, is_ray_depth_21);
            points_12[int(0)] = make_float3 (depths_10.x) * raydir_17;
            float2  uv_63 = (pix_center_13 + make_float2 (1.0f, -0.0f) - _S1156) / _S1157;
            FixedArray<float, 8>  _S1161 = dist_coeffs_25;
            float2  uv_u_32;
            bool _S1162 = undistort_point_2(uv_63, &_S1161, int(12), &uv_u_32);
            if(!_S1162)
            {
                _S1155 = int(0);
                _S1154 = nullptr;
                _S1153 = &points_12[int(0)];
                _S1152 = nullptr;
                _S1151 = nullptr;
                _S1150 = _S1160;
                break;
            }
            float3  raydir_18 = unproject_raydir_0(uv_u_32, camera_model_24, is_ray_depth_21);
            points_12[int(1)] = make_float3 (depths_10.y) * raydir_18;
            float2  uv_64 = (pix_center_13 + make_float2 (0.0f, -1.0f) - _S1156) / _S1157;
            FixedArray<float, 8>  _S1163 = dist_coeffs_25;
            float2  uv_u_33;
            bool _S1164 = undistort_point_2(uv_64, &_S1163, int(12), &uv_u_33);
            if(!_S1164)
            {
                _S1155 = int(0);
                _S1154 = &points_12[int(1)];
                _S1153 = &points_12[int(0)];
                _S1152 = nullptr;
                _S1151 = nullptr;
                _S1150 = _S1160;
                break;
            }
            float3  raydir_19 = unproject_raydir_0(uv_u_33, camera_model_24, is_ray_depth_21);
            points_12[int(2)] = make_float3 (depths_10.z) * raydir_19;
            float2  uv_65 = (pix_center_13 + make_float2 (0.0f, 1.0f) - _S1156) / _S1157;
            FixedArray<float, 8>  _S1165 = dist_coeffs_25;
            float2  uv_u_34;
            bool _S1166 = undistort_point_2(uv_65, &_S1165, int(12), &uv_u_34);
            if(!_S1166)
            {
                _S1155 = int(0);
                _S1154 = &points_12[int(1)];
                _S1153 = &points_12[int(0)];
                _S1152 = nullptr;
                _S1151 = &points_12[int(2)];
                _S1150 = _S1160;
                break;
            }
            float3  raydir_20 = unproject_raydir_0(uv_u_34, camera_model_24, is_ray_depth_21);
            points_12[int(3)] = make_float3 (depths_10.w) * raydir_20;
            float2  uv_66 = (pix_center_13 + make_float2 (0.0f) * make_float2 (0.0f, 3.0f) - _S1156) / _S1157;
            FixedArray<float, 8>  _S1167 = dist_coeffs_25;
            float2  uv_u_35;
            bool _S1168 = undistort_point_2(uv_66, &_S1167, int(12), &uv_u_35);
            if(!_S1168)
            {
                _S1155 = int(0);
                _S1154 = &points_12[int(1)];
                _S1153 = &points_12[int(0)];
                _S1152 = &points_12[int(3)];
                _S1151 = &points_12[int(2)];
                _S1150 = _S1160;
                break;
            }
            float3  raydir_21 = unproject_raydir_0(uv_u_35, camera_model_24, is_ray_depth_21);
            _S1155 = int(1);
            _S1154 = &points_12[int(1)];
            _S1153 = &points_12[int(0)];
            _S1152 = &points_12[int(3)];
            _S1151 = &points_12[int(2)];
            _S1150 = raydir_21;
            break;
        }
        if(_S1155 != int(1))
        {
            _S1149 = 0.0f;
            break;
        }
        float3  normal_14 = cross_0(*_S1154 - *_S1153, - (*_S1152 - *_S1151));
        float3  normal_15;
        if((dot_0(normal_14, normal_14)) != 0.0f)
        {
            normal_15 = normalize_0(normal_14);
        }
        else
        {
            normal_15 = normal_14;
        }
        float3  _S1169;
        if((dot_0(gt_normal_4, gt_normal_4)) != 0.0f)
        {
            _S1169 = normalize_0(gt_normal_4);
        }
        else
        {
            _S1169 = gt_normal_4;
        }
        _S1149 = (1.0f - dot_0(normal_15, _S1169) + 0.00100000004749745f) / ((F32_max((dot_0(normal_15, - normalize_0(_S1150))), (0.0f))) + 0.00100000004749745f);
        break;
    }
    return _S1149;
}

struct s_bwd_prop_depth_normal_loss_Intermediates_2
{
    float2  _S1170;
    bool _S1171;
    float2  _S1172;
    bool _S1173;
    float2  _S1174;
    bool _S1175;
    float2  _S1176;
    bool _S1177;
    float2  _S1178;
    bool _S1179;
};

inline __device__ void depth_normal_loss_vjp_prism(float2  pix_center_14, float4  intrins_23, FixedArray<float, 8>  dist_coeffs_26, int camera_model_25, bool is_ray_depth_22, float4  depths_11, float3  gt_normal_5, float v_loss_2, float4  * v_depths_5, float3  * v_gt_normal_2)
{
    float2  _S1180 = make_float2 (0.0f);
    s_bwd_prop_depth_normal_loss_Intermediates_2 _S1181;
    (&_S1181)->_S1170 = _S1180;
    (&_S1181)->_S1171 = false;
    (&_S1181)->_S1172 = _S1180;
    (&_S1181)->_S1173 = false;
    (&_S1181)->_S1174 = _S1180;
    (&_S1181)->_S1175 = false;
    (&_S1181)->_S1176 = _S1180;
    (&_S1181)->_S1177 = false;
    (&_S1181)->_S1178 = _S1180;
    (&_S1181)->_S1179 = false;
    (&_S1181)->_S1172 = _S1180;
    (&_S1181)->_S1173 = false;
    (&_S1181)->_S1174 = _S1180;
    (&_S1181)->_S1175 = false;
    (&_S1181)->_S1176 = _S1180;
    (&_S1181)->_S1177 = false;
    (&_S1181)->_S1178 = _S1180;
    (&_S1181)->_S1179 = false;
    float2  _S1182 = float2 {intrins_23.z, intrins_23.w};
    float2  _S1183 = float2 {intrins_23.x, intrins_23.y};
    float2  uv_67 = (pix_center_14 + make_float2 (-1.0f, -0.0f) - _S1182) / _S1183;
    float2  _S1184 = _S1180;
    FixedArray<float, 8>  _S1185 = dist_coeffs_26;
    bool _S1186 = undistort_point_2(uv_67, &_S1185, int(12), &_S1184);
    (&_S1181)->_S1170 = _S1184;
    (&_S1181)->_S1171 = _S1186;
    bool _S1187 = !!_S1186;
    bool _runFlag_23;
    if(_S1187)
    {
        float2  uv_68 = (pix_center_14 + make_float2 (1.0f, -0.0f) - _S1182) / _S1183;
        float2  _S1188 = _S1180;
        FixedArray<float, 8>  _S1189 = dist_coeffs_26;
        bool _S1190 = undistort_point_2(uv_68, &_S1189, int(12), &_S1188);
        (&_S1181)->_S1172 = _S1188;
        (&_S1181)->_S1173 = _S1190;
        if(!_S1190)
        {
            _runFlag_23 = false;
        }
        else
        {
            _runFlag_23 = _S1187;
        }
        if(_runFlag_23)
        {
            float2  uv_69 = (pix_center_14 + make_float2 (0.0f, -1.0f) - _S1182) / _S1183;
            float2  _S1191 = _S1180;
            FixedArray<float, 8>  _S1192 = dist_coeffs_26;
            bool _S1193 = undistort_point_2(uv_69, &_S1192, int(12), &_S1191);
            (&_S1181)->_S1174 = _S1191;
            (&_S1181)->_S1175 = _S1193;
            if(!_S1193)
            {
                _runFlag_23 = false;
            }
            if(_runFlag_23)
            {
                float2  uv_70 = (pix_center_14 + make_float2 (0.0f, 1.0f) - _S1182) / _S1183;
                float2  _S1194 = _S1180;
                FixedArray<float, 8>  _S1195 = dist_coeffs_26;
                bool _S1196 = undistort_point_2(uv_70, &_S1195, int(12), &_S1194);
                (&_S1181)->_S1176 = _S1194;
                (&_S1181)->_S1177 = _S1196;
                if(!_S1196)
                {
                    _runFlag_23 = false;
                }
                if(_runFlag_23)
                {
                    float2  uv_71 = (pix_center_14 - _S1182) / _S1183;
                    float2  _S1197 = _S1180;
                    FixedArray<float, 8>  _S1198 = dist_coeffs_26;
                    bool _S1199 = undistort_point_2(uv_71, &_S1198, int(12), &_S1197);
                    (&_S1181)->_S1178 = _S1197;
                    (&_S1181)->_S1179 = _S1199;
                }
            }
        }
    }
    s_bwd_prop_depth_normal_loss_Intermediates_2 _S1200 = _S1181;
    float3  _S1201 = make_float3 (0.0f);
    bool _S1202 = !!_S1181._S1171;
    bool _runFlag_24;
    bool _runFlag_25;
    bool _runFlag_26;
    int _S1203;
    float3  raydir_22;
    float3  _S1204;
    float3  _S1205;
    float3  _S1206;
    float3  _S1207;
    FixedArray<float3 , 5>  points_13;
    if(_S1202)
    {
        float3  _S1208 = s_primal_ctx_unproject_raydir_0(_S1200._S1170, camera_model_25, is_ray_depth_22);
        float3  _S1209 = make_float3 (depths_11.x) * _S1208;
        if(!_S1200._S1173)
        {
            _runFlag_23 = false;
        }
        else
        {
            _runFlag_23 = _S1202;
        }
        if(_runFlag_23)
        {
            float3  _S1210 = s_primal_ctx_unproject_raydir_0(_S1200._S1172, camera_model_25, is_ray_depth_22);
            float3  _S1211 = make_float3 (depths_11.y) * _S1210;
            if(!_S1200._S1175)
            {
                _runFlag_24 = false;
            }
            else
            {
                _runFlag_24 = _runFlag_23;
            }
            if(_runFlag_24)
            {
                float3  _S1212 = s_primal_ctx_unproject_raydir_0(_S1200._S1174, camera_model_25, is_ray_depth_22);
                float3  _S1213 = make_float3 (depths_11.z) * _S1212;
                if(!_S1200._S1177)
                {
                    _runFlag_25 = false;
                }
                else
                {
                    _runFlag_25 = _runFlag_24;
                }
                if(_runFlag_25)
                {
                    float3  _S1214 = s_primal_ctx_unproject_raydir_0(_S1200._S1176, camera_model_25, is_ray_depth_22);
                    float3  _S1215 = make_float3 (depths_11.w) * _S1214;
                    if(!_S1200._S1179)
                    {
                        _runFlag_26 = false;
                    }
                    else
                    {
                        _runFlag_26 = _runFlag_25;
                    }
                    if(_runFlag_26)
                    {
                        float3  _S1216 = s_primal_ctx_unproject_raydir_0(_S1200._S1178, camera_model_25, is_ray_depth_22);
                        _S1203 = int(1);
                        raydir_22 = _S1216;
                    }
                    else
                    {
                        _S1203 = int(0);
                        raydir_22 = _S1214;
                    }
                    points_13[int(0)] = _S1209;
                    points_13[int(1)] = _S1211;
                    points_13[int(2)] = _S1213;
                    points_13[int(3)] = _S1215;
                    points_13[int(4)] = _S1201;
                    _S1204 = _S1214;
                }
                else
                {
                    _S1203 = int(0);
                    raydir_22 = _S1212;
                    points_13[int(0)] = _S1209;
                    points_13[int(1)] = _S1211;
                    points_13[int(2)] = _S1213;
                    points_13[int(3)] = _S1201;
                    points_13[int(4)] = _S1201;
                    _S1204 = _S1201;
                }
                _S1205 = _S1212;
            }
            else
            {
                _S1203 = int(0);
                raydir_22 = _S1210;
                points_13[int(0)] = _S1209;
                points_13[int(1)] = _S1211;
                points_13[int(2)] = _S1201;
                points_13[int(3)] = _S1201;
                points_13[int(4)] = _S1201;
                _runFlag_25 = false;
                _S1204 = _S1201;
                _S1205 = _S1201;
            }
            _S1206 = _S1210;
        }
        else
        {
            _S1203 = int(0);
            raydir_22 = _S1208;
            points_13[int(0)] = _S1209;
            points_13[int(1)] = _S1201;
            points_13[int(2)] = _S1201;
            points_13[int(3)] = _S1201;
            points_13[int(4)] = _S1201;
            _runFlag_24 = false;
            _runFlag_25 = false;
            _S1204 = _S1201;
            _S1205 = _S1201;
            _S1206 = _S1201;
        }
        _S1207 = _S1208;
    }
    else
    {
        _S1203 = int(0);
        points_13[int(0)] = _S1201;
        points_13[int(1)] = _S1201;
        points_13[int(2)] = _S1201;
        points_13[int(3)] = _S1201;
        points_13[int(4)] = _S1201;
        _runFlag_23 = false;
        _runFlag_24 = false;
        _runFlag_25 = false;
        _S1204 = _S1201;
        _S1205 = _S1201;
        _S1206 = _S1201;
        _S1207 = _S1201;
    }
    bool _S1217 = !(_S1203 != int(1));
    bool _S1218;
    float3  normal_16;
    float3  _S1219;
    float3  _S1220;
    float3  _S1221;
    float3  _S1222;
    float _S1223;
    float _S1224;
    float _S1225;
    float _S1226;
    if(_S1217)
    {
        float3  dx_6 = points_13[int(1)] - points_13[int(0)];
        float3  _S1227 = - (points_13[int(3)] - points_13[int(2)]);
        float3  _S1228 = s_primal_ctx_cross_0(dx_6, _S1227);
        bool _S1229 = (s_primal_ctx_dot_0(_S1228, _S1228)) != 0.0f;
        if(_S1229)
        {
            normal_16 = normalize_0(_S1228);
        }
        else
        {
            normal_16 = _S1228;
        }
        bool _S1230 = (s_primal_ctx_dot_0(gt_normal_5, gt_normal_5)) != 0.0f;
        if(_S1230)
        {
            _S1219 = normalize_0(gt_normal_5);
        }
        else
        {
            _S1219 = gt_normal_5;
        }
        float3  _S1231 = - normalize_0(raydir_22);
        float _S1232 = s_primal_ctx_dot_0(normal_16, _S1231);
        float _S1233 = 1.0f - s_primal_ctx_dot_0(normal_16, _S1219) + 0.00100000004749745f;
        float _S1234 = (F32_max((_S1232), (0.0f))) + 0.00100000004749745f;
        _S1223 = _S1234 * _S1234;
        _S1224 = _S1233;
        _S1225 = _S1234;
        _S1226 = _S1232;
        raydir_22 = normal_16;
        normal_16 = _S1231;
        _runFlag_26 = _S1230;
        _S1218 = _S1229;
        _S1220 = _S1228;
        _S1221 = dx_6;
        _S1222 = _S1227;
    }
    else
    {
        _S1223 = 0.0f;
        _S1224 = 0.0f;
        _S1225 = 0.0f;
        _S1226 = 0.0f;
        raydir_22 = _S1201;
        normal_16 = _S1201;
        _S1219 = _S1201;
        _runFlag_26 = false;
        _S1218 = false;
        _S1220 = _S1201;
        _S1221 = _S1201;
        _S1222 = _S1201;
    }
    float4  _S1235 = make_float4 (0.0f);
    if(_S1217)
    {
        float _S1236 = v_loss_2 / _S1223;
        float _S1237 = _S1224 * - _S1236;
        float s_diff_num_T_2 = _S1225 * _S1236;
        DiffPair_float_0 _S1238;
        (&_S1238)->primal_0 = _S1226;
        (&_S1238)->differential_0 = 0.0f;
        DiffPair_float_0 _S1239;
        (&_S1239)->primal_0 = 0.0f;
        (&_S1239)->differential_0 = 0.0f;
        _d_max_0(&_S1238, &_S1239, _S1237);
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S1240;
        (&_S1240)->primal_0 = raydir_22;
        (&_S1240)->differential_0 = _S1201;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S1241;
        (&_S1241)->primal_0 = normal_16;
        (&_S1241)->differential_0 = _S1201;
        s_bwd_prop_dot_0(&_S1240, &_S1241, _S1238.differential_0);
        float _S1242 = - s_diff_num_T_2;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S1243;
        (&_S1243)->primal_0 = raydir_22;
        (&_S1243)->differential_0 = _S1201;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S1244;
        (&_S1244)->primal_0 = _S1219;
        (&_S1244)->differential_0 = _S1201;
        s_bwd_prop_dot_0(&_S1243, &_S1244, _S1242);
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S1245 = _S1244;
        float3  _S1246 = _S1240.differential_0 + _S1243.differential_0;
        if(_runFlag_26)
        {
            DiffPair_vectorx3Cfloatx2C3x3E_0 _S1247;
            (&_S1247)->primal_0 = gt_normal_5;
            (&_S1247)->differential_0 = _S1201;
            s_bwd_normalize_impl_0(&_S1247, _S1245.differential_0);
            raydir_22 = _S1247.differential_0;
        }
        else
        {
            raydir_22 = _S1245.differential_0;
        }
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S1248;
        (&_S1248)->primal_0 = gt_normal_5;
        (&_S1248)->differential_0 = _S1201;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S1249;
        (&_S1249)->primal_0 = gt_normal_5;
        (&_S1249)->differential_0 = _S1201;
        s_bwd_prop_dot_0(&_S1248, &_S1249, 0.0f);
        float3  _S1250 = _S1249.differential_0 + _S1248.differential_0 + raydir_22;
        if(_S1218)
        {
            DiffPair_vectorx3Cfloatx2C3x3E_0 _S1251;
            (&_S1251)->primal_0 = _S1220;
            (&_S1251)->differential_0 = _S1201;
            s_bwd_normalize_impl_0(&_S1251, _S1246);
            raydir_22 = _S1251.differential_0;
        }
        else
        {
            raydir_22 = _S1246;
        }
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S1252;
        (&_S1252)->primal_0 = _S1220;
        (&_S1252)->differential_0 = _S1201;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S1253;
        (&_S1253)->primal_0 = _S1220;
        (&_S1253)->differential_0 = _S1201;
        s_bwd_prop_dot_0(&_S1252, &_S1253, 0.0f);
        float3  _S1254 = _S1253.differential_0 + _S1252.differential_0 + raydir_22;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S1255;
        (&_S1255)->primal_0 = _S1221;
        (&_S1255)->differential_0 = _S1201;
        DiffPair_vectorx3Cfloatx2C3x3E_0 _S1256;
        (&_S1256)->primal_0 = _S1222;
        (&_S1256)->differential_0 = _S1201;
        s_bwd_prop_cross_0(&_S1255, &_S1256, _S1254);
        float3  s_diff_dy_T_6 = - _S1256.differential_0;
        float3  _S1257 = - s_diff_dy_T_6;
        float3  _S1258 = - _S1255.differential_0;
        FixedArray<float3 , 5>  _S1259;
        _S1259[int(0)] = _S1201;
        _S1259[int(1)] = _S1201;
        _S1259[int(2)] = _S1201;
        _S1259[int(3)] = _S1201;
        _S1259[int(4)] = _S1201;
        _S1259[int(2)] = _S1257;
        _S1259[int(3)] = s_diff_dy_T_6;
        _S1259[int(0)] = _S1258;
        _S1259[int(1)] = _S1255.differential_0;
        points_13[int(0)] = _S1259[int(0)];
        points_13[int(1)] = _S1259[int(1)];
        points_13[int(2)] = _S1259[int(2)];
        points_13[int(3)] = _S1259[int(3)];
        points_13[int(4)] = _S1259[int(4)];
        raydir_22 = _S1250;
    }
    else
    {
        points_13[int(0)] = _S1201;
        points_13[int(1)] = _S1201;
        points_13[int(2)] = _S1201;
        points_13[int(3)] = _S1201;
        points_13[int(4)] = _S1201;
        raydir_22 = _S1201;
    }
    float4  _S1260;
    if(_S1202)
    {
        if(_runFlag_23)
        {
            if(_runFlag_24)
            {
                if(_runFlag_25)
                {
                    FixedArray<float3 , 5>  _S1261 = points_13;
                    FixedArray<float3 , 5>  _S1262 = points_13;
                    FixedArray<float3 , 5>  _S1263 = points_13;
                    float3  _S1264 = _S1204 * points_13[int(3)];
                    float _S1265 = _S1264.x + _S1264.y + _S1264.z;
                    float4  _S1266 = _S1235;
                    *&((&_S1266)->w) = _S1265;
                    points_13[int(0)] = _S1201;
                    points_13[int(1)] = _S1201;
                    points_13[int(2)] = _S1201;
                    points_13[int(3)] = _S1201;
                    points_13[int(4)] = _S1201;
                    _S1204 = _S1263[int(2)];
                    normal_16 = _S1261[int(0)];
                    _S1219 = _S1262[int(1)];
                    _S1260 = _S1266;
                }
                else
                {
                    FixedArray<float3 , 5>  _S1267 = points_13;
                    FixedArray<float3 , 5>  _S1268 = points_13;
                    FixedArray<float3 , 5>  _S1269 = points_13;
                    FixedArray<float3 , 5>  _S1270 = points_13;
                    points_13[int(0)] = points_13[int(0)];
                    points_13[int(1)] = _S1267[int(1)];
                    points_13[int(2)] = _S1268[int(2)];
                    points_13[int(3)] = _S1269[int(3)];
                    points_13[int(4)] = _S1270[int(4)];
                    _S1204 = _S1201;
                    normal_16 = _S1201;
                    _S1219 = _S1201;
                    _S1260 = _S1235;
                }
                float3  _S1271 = _S1205 * (points_13[int(2)] + _S1204);
                float _S1272 = _S1271.x + _S1271.y + _S1271.z;
                float3  _S1273 = points_13[int(0)] + normal_16;
                float3  _S1274 = points_13[int(1)] + _S1219;
                float4  _S1275 = _S1235;
                *&((&_S1275)->z) = _S1272;
                float4  _S1276 = _S1260 + _S1275;
                points_13[int(0)] = _S1201;
                points_13[int(1)] = _S1201;
                points_13[int(2)] = _S1201;
                points_13[int(3)] = _S1201;
                points_13[int(4)] = _S1201;
                _S1204 = _S1274;
                _S1205 = _S1273;
                _S1260 = _S1276;
            }
            else
            {
                FixedArray<float3 , 5>  _S1277 = points_13;
                FixedArray<float3 , 5>  _S1278 = points_13;
                FixedArray<float3 , 5>  _S1279 = points_13;
                FixedArray<float3 , 5>  _S1280 = points_13;
                points_13[int(0)] = points_13[int(0)];
                points_13[int(1)] = _S1277[int(1)];
                points_13[int(2)] = _S1278[int(2)];
                points_13[int(3)] = _S1279[int(3)];
                points_13[int(4)] = _S1280[int(4)];
                _S1204 = _S1201;
                _S1205 = _S1201;
                _S1260 = _S1235;
            }
            float3  _S1281 = _S1206 * (points_13[int(1)] + _S1204);
            float _S1282 = _S1281.x + _S1281.y + _S1281.z;
            float3  _S1283 = points_13[int(0)] + _S1205;
            float4  _S1284 = _S1235;
            *&((&_S1284)->y) = _S1282;
            float4  _S1285 = _S1260 + _S1284;
            points_13[int(0)] = _S1201;
            points_13[int(1)] = _S1201;
            points_13[int(2)] = _S1201;
            points_13[int(3)] = _S1201;
            points_13[int(4)] = _S1201;
            _S1204 = _S1283;
            _S1260 = _S1285;
        }
        else
        {
            FixedArray<float3 , 5>  _S1286 = points_13;
            FixedArray<float3 , 5>  _S1287 = points_13;
            FixedArray<float3 , 5>  _S1288 = points_13;
            FixedArray<float3 , 5>  _S1289 = points_13;
            points_13[int(0)] = points_13[int(0)];
            points_13[int(1)] = _S1286[int(1)];
            points_13[int(2)] = _S1287[int(2)];
            points_13[int(3)] = _S1288[int(3)];
            points_13[int(4)] = _S1289[int(4)];
            _S1204 = _S1201;
            _S1260 = _S1235;
        }
        float3  _S1290 = _S1207 * (points_13[int(0)] + _S1204);
        float _S1291 = _S1290.x + _S1290.y + _S1290.z;
        float4  _S1292 = _S1235;
        *&((&_S1292)->x) = _S1291;
        _S1260 = _S1260 + _S1292;
    }
    else
    {
        _S1260 = _S1235;
    }
    *v_depths_5 = _S1260;
    *v_gt_normal_2 = raydir_22;
    return;
}

