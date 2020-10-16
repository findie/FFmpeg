#define DEBUG
#define DEBUG_CENTER
#undef DEBUG
#undef DEBUG_CENTER

__constant sampler_t sampler = (CLK_NORMALIZED_COORDS_FALSE |
                                CLK_FILTER_LINEAR           |
                                CLK_ADDRESS_NONE);

static inline float2 scale_coords_pxout_to_pxin(float2 pix_out, float2 dim_out, float ZOOM, float2 dim_in, float2 PAN) {

    if (ZOOM < 1) {
        //                                               canvas offset   obj scaled center offset   scaled px location
        // px_out                                      = dim_out * PAN - dim_in / 2 * ZOOM        + px_in * ZOOM

        // -dim_out * PAN + px_out                     = (-dim_in/2 + px_in) * ZOOM

        // (-dim_out * PAN + px_out) / ZOOM            = -dim_in/2 + px_in

        // (-dim_out * PAN + px_out) / ZOOM + dim_in/2 = px_in



        return (-dim_out * PAN + pix_out) / ZOOM + dim_in / 2;
    }

    // zoom >= 1
    else {
        return (pix_out - dim_out/2) / ZOOM + dim_in * PAN;
    }

}

static inline float2 scale_coords_find_PAN(float2 pix_in, float2 pix_out, float2 dim_out, float ZOOM, float2 dim_in) {
    if(ZOOM < 1){
        // taken from scale_coords_pxout_to_pxin
        // pix_in                                                = (-dim_out * PAN + pix_out) / ZOOM + dim_in / 2;
        // pix_in - dim_in / 2                                   = (-dim_out * PAN + pix_out) / ZOOM
        // (pix_in - dim_in / 2) * ZOOM                          = -dim_out * PAN + pix_out
        // (pix_in - dim_in / 2) * ZOOM - pix_out                = -dim_out * PAN
        // ((pix_in - dim_in / 2) * ZOOM - pix_out) / (-dim_out) = PAN
        return ((pix_in - dim_in / 2) * ZOOM - pix_out) / (-dim_out);
    }
    // zoom >= 1
    else {
        // taken from scale_coords_pxout_to_pxin
        // pix_in                                           = (pix_out - dim_out/2) / ZOOM + dim_in * PAN
        // pix_in - (pix_out - dim_out/2) / ZOOM            = dim_in * PAN
        // (pix_in - (pix_out - dim_out/2) / ZOOM) / dim_in = PAN
        return (pix_in - (pix_out - dim_out/2) / ZOOM) / dim_in;
    }
}

static inline float2 clamp_pan_inbounds(float2 PAN, float2 dim_out, float ZOOM, float2 dim_in) {

    float2 adjusted_dim_in = dim_in * ZOOM;
    float2 top_left = scale_coords_find_PAN((0.0f, 0.0f), (0.0f, 0.0f), dim_out, ZOOM, dim_in);
    float2 bottom_right = 1 - top_left;

    float2 CLAMPED_PAN = (0.0f, 0.0f);

    if(ZOOM < 1) {
        // if it fits
        if(adjusted_dim_in.x <= dim_out.x && adjusted_dim_in.y <= dim_out.y) {
            CLAMPED_PAN.x = clamp(PAN.x, top_left.x, bottom_right.x);
            CLAMPED_PAN.y = clamp(PAN.y, top_left.y, bottom_right.y);
        }
        // if it doesn't fit
        else {

            CLAMPED_PAN.x = adjusted_dim_in.x > dim_out.x ?
                // doesn't fit on W
                clamp(1.0f - PAN.x, 0.0f, 1.0f) * (top_left.x - bottom_right.x) + bottom_right.x :
                // fits on W
                max(min(PAN.x, bottom_right.x), top_left.x);
            CLAMPED_PAN.y = adjusted_dim_in.y > dim_out.y ?
                // doesn't fit on H
                clamp(1.0f - PAN.y, 0.0f, 1.0f) * (top_left.y - bottom_right.y) + bottom_right.y :
                // fits on H
                max(min(PAN.y, bottom_right.y), top_left.y);

        }
    } else {

        CLAMPED_PAN.x = clamp(PAN.x, top_left.x, bottom_right.x);
        CLAMPED_PAN.y = clamp(PAN.y, top_left.y, bottom_right.y);
    }

    return CLAMPED_PAN;
}

__kernel void zoom(__write_only image2d_t destination,
                   unsigned int index,
                   float2 UNCLAMPED_PAN,
                   float ZOOM,
                   float SHADOW_ZOOM,
                   float oob_plane_color,
                   __read_only  image2d_t source)
{
    float2 dim_in  = (float2)(get_image_dim(source).x, get_image_dim(source).y);
    float2 dim_out = (float2)(get_image_dim(destination).x / SHADOW_ZOOM, get_image_dim(destination).y / SHADOW_ZOOM);

    float2 PAN = clamp_pan_inbounds(UNCLAMPED_PAN, dim_out, ZOOM, dim_in);

    int2 dst_location = (int2)(get_global_id(0), get_global_id(1));

    float2 src_location = scale_coords_pxout_to_pxin(
        (float2)(get_global_id(0), get_global_id(1)) / SHADOW_ZOOM,
        dim_out,
        ZOOM,
        dim_in,
        PAN
    );

#ifdef DEBUG
    if(dst_location.x == 0 && dst_location.y == 0) {
        float2 top_left = scale_coords_find_PAN((0.0f, 0.0f), (0.0f, 0.0f), dim_out, ZOOM, dim_in);
        float2 bottom_right = 1 - top_left;

        printf("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");
        printf("ZOOM %.3f\n", ZOOM);
        printf("dim_in x %.3f y %.3f\n",dim_in.x,dim_in.y);
        printf("dim_out x %.3f y %.3f\n",dim_out.x,dim_out.y);
        printf("UNCLAMPED_PAN x %.3f y %.3f\n",UNCLAMPED_PAN.x,UNCLAMPED_PAN.y);
        printf("TOP_LEFT X %f Y %f \n", top_left.x, top_left.y);
        printf("BOTTOM_RIGHT X %f Y %f \n", bottom_right.x, bottom_right.y);
        printf("PAN x %.3f y %.3f\n",PAN.x,PAN.y);
        printf("src_location x %.3f y %.3f\n",src_location.x,src_location.y);
        printf("dst_location x %d y %d\n",dst_location.x,dst_location.y);
    }
#endif

    bool oob = src_location.x < 0 || src_location.y < 0 || src_location.x > dim_in.x-1 || src_location.y > dim_in.y-1;
    float4 value = oob ? oob_plane_color : read_imagef(source, sampler, src_location);

    write_imagef(destination, dst_location, value);


#ifdef DEBUG_CENTER
    // DRAW CENTER GUIDES DEBUG
    if( fabs(dst_location.x - dim_out.x/2) < 2 ||
        fabs(dst_location.y - dim_out.y/2) < 2 )

        write_imagef(destination, dst_location, (float4){0.5f,0.5f,0.5f,0});

    // DRAW PAN DEBUG
    if( fabs(dst_location.x - dim_out.x * UNCLAMPED_PAN.x) < 10 ||
        fabs(dst_location.y - dim_out.y * UNCLAMPED_PAN.y) < 10 )

        write_imagef(destination, dst_location, (float4){0.5f,0.5f,0.5f,0});
#endif
}