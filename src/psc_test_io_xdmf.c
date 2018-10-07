
#define BOUNDS_CHECK
#define BOUNDSCHECK

#include <mrc_io_private.h>
#include <mrc_params.h>
#include "../src/mrc_io_xdmf_lib.h"

#include <hdf5.h>
#include <hdf5_hl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define H5_CHK(ierr) assert(ierr >= 0)
#define CE assert(ierr == 0)

struct xdmf_file {
  hid_t h5_file;
  list_t xdmf_spatial_list;
};

struct xdmf {
  int slab_dims[3];
  int slab_off[3];
  struct xdmf_file file;
  struct xdmf_temporal *xdmf_temporal;
  bool use_independent_io;
  char *romio_cb_write;
  char *romio_ds_write;
  int nr_writers;
  MPI_Comm comm_writers; //< communicator for only the writers
  int *writers;          //< rank (in mrc_io comm) for each writer
  int is_writer;         //< this rank is a writer
  char *mode;            //< open mode, "r" or "w"
};

#define VAR(x) (void *)offsetof(struct xdmf, x)
static struct param xdmf_collective_descr[] = {
  { "use_independent_io"     , VAR(use_independent_io)      , PARAM_BOOL(false)      },
  { "nr_writers"             , VAR(nr_writers)              , PARAM_INT(1)           },
  { "romio_cb_write"         , VAR(romio_cb_write)          , PARAM_STRING(NULL)     },
  { "romio_ds_write"         , VAR(romio_ds_write)          , PARAM_STRING(NULL)     },
  { "slab_dims"              , VAR(slab_dims)               , PARAM_INT3(0, 0, 0)    },
  { "slab_off"               , VAR(slab_off)                , PARAM_INT3(0, 0, 0)    },
  {},
};
#undef VAR

#define to_xdmf(io) mrc_to_subobj(io, struct xdmf)

// ----------------------------------------------------------------------
// xdmf_collective_setup

static void
xdmf_collective_setup(struct mrc_io *io)
{
  mrc_io_setup_super(io);

  struct xdmf *xdmf = to_xdmf(io);

  char filename[strlen(io->par.outdir) + strlen(io->par.basename) + 7];
  sprintf(filename, "%s/%s.xdmf", io->par.outdir, io->par.basename);
  xdmf->xdmf_temporal = xdmf_temporal_create(filename);

#ifndef H5_HAVE_PARALLEL
  assert(xdmf->nr_writers == 1);
#endif
  
  if (xdmf->nr_writers > io->size) {
    xdmf->nr_writers = io->size;
  }
  xdmf->writers = calloc(xdmf->nr_writers, sizeof(*xdmf->writers));
  // setup writers, just use first nr_writers ranks,
  // could do something fancier in the future
  for (int i = 0; i < xdmf->nr_writers; i++) {
    xdmf->writers[i] = i;
    if (i == io->rank)
      xdmf->is_writer = 1;
  }
  MPI_Comm_split(mrc_io_comm(io), xdmf->is_writer, io->rank, &xdmf->comm_writers);
}

// ----------------------------------------------------------------------
// xdmf_collective_destroy

static void
xdmf_collective_destroy(struct mrc_io *io)
{
  struct xdmf *xdmf = to_xdmf(io);
  
  free(xdmf->writers);
  if (xdmf->comm_writers) {
    MPI_Comm_free(&xdmf->comm_writers);
  }

  if (xdmf->xdmf_temporal) {
    xdmf_temporal_destroy(xdmf->xdmf_temporal);
  }
}

// ----------------------------------------------------------------------
// xdmf_collective_open

static void
xdmf_collective_open(struct mrc_io *io, const char *mode)
{
  struct xdmf *xdmf = to_xdmf(io);
  struct xdmf_file *file = &xdmf->file;
  xdmf->mode = strdup(mode);
  //  assert(strcmp(mode, "w") == 0);

  char filename[strlen(io->par.outdir) + strlen(io->par.basename) + 20];
  sprintf(filename, "%s/%s.%06d_p%06d.h5", io->par.outdir, io->par.basename,
	  io->step, 0);

  if (xdmf->is_writer) {
    hid_t plist = H5Pcreate(H5P_FILE_ACCESS);
    MPI_Info info;
    MPI_Info_create(&info);
    if (xdmf->romio_cb_write) {
      MPI_Info_set(info, "romio_cb_write", xdmf->romio_cb_write);
    }
    if (xdmf->romio_ds_write) {
      MPI_Info_set(info, "romio_ds_write", xdmf->romio_ds_write);
    }
#ifdef H5_HAVE_PARALLEL
    H5Pset_fapl_mpio(plist, xdmf->comm_writers, info);
#endif
    if (strcmp(mode, "w") == 0) {
      file->h5_file = H5Fcreate(filename, H5F_ACC_TRUNC, H5P_DEFAULT, plist);
    } else if (strcmp(mode, "r") == 0) {
      file->h5_file = H5Fopen(filename, H5F_ACC_RDONLY, plist);
    } else {
      assert(0);
    }
    H5Pclose(plist);
    MPI_Info_free(&info);
  }
  xdmf_spatial_open(&file->xdmf_spatial_list);
}

// ----------------------------------------------------------------------
// xdmf_collective_close

static void
xdmf_collective_close(struct mrc_io *io)
{
  struct xdmf *xdmf = to_xdmf(io);
  struct xdmf_file *file = &xdmf->file;

  xdmf_spatial_close(&file->xdmf_spatial_list, io, xdmf->xdmf_temporal);
  if (xdmf->is_writer) {
    H5Fclose(file->h5_file);
    memset(file, 0, sizeof(*file));
  }
  free(xdmf->mode);
  xdmf->mode = NULL;
}

// ======================================================================

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

static bool
find_intersection(int *ilo, int *ihi, const int *ib1, const int *im1,
		  const int *ib2, const int *im2)
{
  bool has_intersection = true;
  for (int d = 0; d < 3; d++) {
    ilo[d] = MAX(ib1[d], ib2[d]);
    ihi[d] = MIN(ib1[d] + im1[d], ib2[d] + im2[d]);
    if (ihi[d] - ilo[d] <= 0) {
      has_intersection = false;
    }
  }
  return has_intersection;
}

// ----------------------------------------------------------------------
// collective helper context

struct collective_m3_entry {
};

struct collective_m3_recv_patch {
  int ilo[3]; // intersection low
  int ihi[3]; // intersection high
};

struct collective_m3_peer {
  int rank;
  struct collective_m3_recv_patch *begin;
  struct collective_m3_recv_patch *end;
  void *recv_buf;
  size_t buf_size;
};

struct collective_m3_ctx {
  int gdims[3];
  int slab_dims[3], slab_off[3];
  int nr_patches, nr_global_patches;
  int slow_dim;
  int slow_indices_per_writer;
  int slow_indices_rmndr;

  struct collective_m3_recv_patch *recv_patches;
  int n_recv_patches;

  struct collective_m3_peer *peers;
  int n_peers;

  struct collective_m3_entry *sends;
  void **send_bufs; // one for each writer
  MPI_Request *send_reqs;
  int nr_sends;

  struct collective_m3_entry *recvs;
  MPI_Request *recv_reqs;
  int nr_recvs;
};

static void
collective_m3_init(struct mrc_io *io, struct collective_m3_ctx *ctx,
		   struct mrc_domain *domain)
{
  struct xdmf *xdmf = to_xdmf(io);

  mrc_domain_get_global_dims(domain, ctx->gdims);
  mrc_domain_get_patches(domain, &ctx->nr_patches);
  mrc_domain_get_nr_global_patches(domain, &ctx->nr_global_patches);
  for (int d = 0; d < 3; d++) {
    if (xdmf->slab_dims[d]) {
      ctx->slab_dims[d] = xdmf->slab_dims[d];
    } else {
      ctx->slab_dims[d] = ctx->gdims[d];
    }
    ctx->slab_off[d] = xdmf->slab_off[d];
  }
  ctx->slow_dim = 2;
  while (ctx->gdims[ctx->slow_dim] == 1) {
    ctx->slow_dim--;
  }
  assert(ctx->slow_dim >= 0);
  int total_slow_indices = ctx->slab_dims[ctx->slow_dim];
  ctx->slow_indices_per_writer = total_slow_indices / xdmf->nr_writers;
  ctx->slow_indices_rmndr = total_slow_indices % xdmf->nr_writers;
}

static void
get_writer_off_dims(struct collective_m3_ctx *ctx, int writer,
		    int *writer_off, int *writer_dims)
{
  for (int d = 0; d < 3; d++) {
    writer_dims[d] = ctx->slab_dims[d];
    writer_off[d] = ctx->slab_off[d];
  }
  writer_dims[ctx->slow_dim] = ctx->slow_indices_per_writer + (writer < ctx->slow_indices_rmndr);
  if (writer < ctx->slow_indices_rmndr) {
    writer_off[ctx->slow_dim] += (ctx->slow_indices_per_writer + 1) * writer;
  } else {
    writer_off[ctx->slow_dim] += ctx->slow_indices_rmndr +
      ctx->slow_indices_per_writer * writer;
  }
}

// ----------------------------------------------------------------------
// collective_send_fld_begin
#define BUFLOOP(ix, iy, iz, ilo, hi) \
      for (int iz = ilo[2]; iz < ihi[2]; iz++) {\
	for (int iy = ilo[1]; iy < ihi[1]; iy++) {\
	  for (int ix = ilo[0]; ix < ihi[0]; ix++)

#define BUFLOOP_END }}
static void
collective_send_fld_begin(struct collective_m3_ctx *ctx, struct mrc_io *io,
			  struct mrc_fld *m3, int m)
{
  struct xdmf *xdmf = to_xdmf(io);

  int nr_patches;
  struct mrc_patch *patches = mrc_domain_get_patches(m3->_domain, &nr_patches);

  size_t *buf_sizes = calloc(xdmf->nr_writers, sizeof(*buf_sizes));
  ctx->send_bufs = calloc(xdmf->nr_writers, sizeof(*ctx->send_bufs));
  ctx->send_reqs = calloc(xdmf->nr_writers, sizeof(*ctx->send_reqs));

  for (int writer = 0; writer < xdmf->nr_writers; writer++) {
    // don't send to self
    if (xdmf->writers[writer] == io->rank) {
      ctx->send_reqs[writer] = MPI_REQUEST_NULL;
      continue;
    }
    int writer_off[3], writer_dims[3];
    get_writer_off_dims(ctx, writer, writer_off, writer_dims);

    // find buf_size per writer
    for (int p = 0; p < nr_patches; p++) {
      int ilo[3], ihi[3];
      bool has_intersection =
	find_intersection(ilo, ihi, patches[p].off, patches[p].ldims,
			  writer_off, writer_dims);
      if (!has_intersection)
	continue;

      size_t len = (size_t) m3->_dims.vals[0] * m3->_dims.vals[1] * m3->_dims.vals[2];
      buf_sizes[writer] += len;
    }

    // allocate buf per writer
    //mprintf("to writer %d buf_size %d\n", writer, buf_sizes[writer]);
    if (buf_sizes[writer] == 0) {
      ctx->send_reqs[writer] = MPI_REQUEST_NULL;
      continue;
    }

    ctx->send_bufs[writer] = malloc(buf_sizes[writer] * m3->_nd->size_of_type);
    assert(ctx->send_bufs[writer]);
    
    assert(mrc_fld_data_type(m3) == MRC_NT_FLOAT);
    MPI_Datatype mpi_dtype = MPI_FLOAT;

    MPI_Isend(ctx->send_bufs[writer], buf_sizes[writer], mpi_dtype,
	      xdmf->writers[writer], 0x1000, mrc_io_comm(io),
	      &ctx->send_reqs[writer]);
  }
  free(buf_sizes);
}

// ----------------------------------------------------------------------
// collective_send_fld_end

static void
collective_send_fld_end(struct collective_m3_ctx *ctx, struct mrc_io *io,
			struct mrc_fld *m3, int m)
{
  struct xdmf *xdmf = to_xdmf(io);

  MHERE;
  MPI_Waitall(xdmf->nr_writers, ctx->send_reqs, MPI_STATUSES_IGNORE);

  for (int writer = 0; writer < xdmf->nr_writers; writer++) {
    free(ctx->send_bufs[writer]);
  }
  free(ctx->send_bufs);
  free(ctx->send_reqs);
}
    
// ----------------------------------------------------------------------
// writer_comm_init

static void
writer_comm_init(struct collective_m3_ctx *ctx,
		 struct mrc_io *io, struct mrc_ndarray *nd,
		 struct mrc_domain *domain, int size_of_type)
{
  // find out who's sending, OPT: this way is not very scalable
  // could also be optimized by just looking at slow_dim
  // FIXME, figure out pattern and cache, at least across components

  int nr_global_patches;
  mrc_domain_get_nr_global_patches(domain, &nr_global_patches);

  ctx->n_recv_patches = 0;
  for (int gp = 0; gp < nr_global_patches; gp++) {
    struct mrc_patch_info info;
    mrc_domain_get_global_patch_info(domain, gp, &info);

    int ilo[3], ihi[3];
    int has_intersection = find_intersection(ilo, ihi, info.off, info.ldims,
					     mrc_ndarray_offs(nd), mrc_ndarray_dims(nd));
    if (!has_intersection) {
      continue;
    }

    ctx->n_recv_patches++;
  }
  mprintf("n_recv_patches %d\n", ctx->n_recv_patches);

  ctx->recv_patches = calloc(ctx->n_recv_patches, sizeof(*ctx->recv_patches));

  struct collective_m3_recv_patch **recv_patches_by_rank = calloc(io->size + 1, sizeof(*recv_patches_by_rank));

  int cur_rank = -1;
  ctx->n_recv_patches = 0;
  for (int gp = 0; gp < nr_global_patches; gp++) {
    struct mrc_patch_info info;
    mrc_domain_get_global_patch_info(domain, gp, &info);

    int ilo[3], ihi[3];
    int has_intersection = find_intersection(ilo, ihi, info.off, info.ldims,
					     mrc_ndarray_offs(nd), mrc_ndarray_dims(nd));
    if (!has_intersection) {
      continue;
    }

    assert(info.rank >= cur_rank);
    while (cur_rank < info.rank) {
      cur_rank++;
      recv_patches_by_rank[cur_rank] = &ctx->recv_patches[ctx->n_recv_patches];
      //mprintf("rank %d patches start at %d\n", cur_rank, ctx->n_recv_patches);
    }

    struct collective_m3_recv_patch *recv_patch = &ctx->recv_patches[ctx->n_recv_patches];
    for (int d = 0; d < 3; d++) {
      recv_patch->ilo[d] = ilo[d];
      recv_patch->ihi[d] = ihi[d];
    }
    ctx->n_recv_patches++;
  }

  while (cur_rank < io->size) {
    cur_rank++;
    recv_patches_by_rank[cur_rank] = &ctx->recv_patches[ctx->n_recv_patches];
    //mprintf("rank %d patches start at %d\n", cur_rank, ctx->n_recv_patches);
  }

  ctx->n_peers = 0;
  for (int rank = 0; rank < io->size; rank++) {
    struct collective_m3_recv_patch *begin = recv_patches_by_rank[rank];
    struct collective_m3_recv_patch *end   = recv_patches_by_rank[rank+1];

    if (begin == end) {
      continue;
    }

    //mprintf("peer rank %d # = %ld\n", rank, end - begin);
    ctx->n_peers++;
  }
  mprintf("n_peers %d\n", ctx->n_peers);

  ctx->peers = calloc(ctx->n_peers, sizeof(*ctx->peers));
  ctx->n_peers = 0;
  for (int rank = 0; rank < io->size; rank++) {
    struct collective_m3_recv_patch *begin = recv_patches_by_rank[rank];
    struct collective_m3_recv_patch *end   = recv_patches_by_rank[rank+1];

    if (begin == end) {
      continue;
    }

    struct collective_m3_peer *peer = &ctx->peers[ctx->n_peers];
    peer->rank = rank;
    peer->begin = begin;
    peer->end = end;

    // for remote patches, allocate buffer
    if (peer->rank != io->rank) {
      peer->buf_size = 0;
      for (struct collective_m3_recv_patch *recv_patch = peer->begin; recv_patch < peer->end; recv_patch++) {
	int *ilo = recv_patch->ilo, *ihi = recv_patch->ihi;
	peer->buf_size += (size_t) (ihi[0] - ilo[0]) * (ihi[1] - ilo[1]) * (ihi[2] - ilo[2]);
      }
      
      // alloc aggregate recv buffers
      peer->recv_buf = malloc(peer->buf_size * size_of_type);
    }

    ctx->n_peers++;
  }
  free(recv_patches_by_rank);

  ctx->recv_reqs = calloc(ctx->n_peers, sizeof(*ctx->recv_reqs));
  mprintf("recv_reqs %p\n", ctx->recv_reqs);
}

// ----------------------------------------------------------------------
// writer_comm_begin

static void
writer_comm_begin(struct collective_m3_ctx *ctx, struct mrc_io *io, struct mrc_ndarray *nd,
		  struct mrc_fld *m3)
{
  for (struct collective_m3_peer *peer = ctx->peers; peer < ctx->peers + ctx->n_peers; peer++) {
    // skip local patches
    if (peer->rank == io->rank) {
      ctx->recv_reqs[peer - ctx->peers] = MPI_REQUEST_NULL;
      continue;
    }

    assert (mrc_fld_data_type(m3) == MRC_NT_FLOAT);
    MPI_Datatype mpi_dtype = MPI_FLOAT;
    
    // recv aggregate buffers
    MPI_Irecv(peer->recv_buf, peer->buf_size, mpi_dtype, peer->rank, 0x1000, mrc_io_comm(io),
	      &ctx->recv_reqs[peer - ctx->peers]);
  }
}

// ----------------------------------------------------------------------
// writer_comm_end

static void
writer_comm_end(struct collective_m3_ctx *ctx, struct mrc_io *io, struct mrc_ndarray *nd,
		struct mrc_fld *m3, int m)
{
  MHERE;
  MPI_Waitall(ctx->n_peers, ctx->recv_reqs, MPI_STATUSES_IGNORE);

  for (struct collective_m3_peer *peer = ctx->peers; peer < ctx->peers + ctx->n_peers; peer++) {
    // skip local patches
    if (peer->rank == io->rank) {
      continue;
    }

    assert(mrc_fld_data_type(m3) == MRC_NT_FLOAT);
    {
      float *buf = peer->recv_buf;
      for (struct collective_m3_recv_patch *recv_patch = peer->begin; recv_patch < peer->end; recv_patch++) {
	int *ilo = recv_patch->ilo, *ihi = recv_patch->ihi;
	BUFLOOP(ix, iy, iz, ilo, ihi) {
	  MRC_S3(nd, ix,iy,iz) = *buf++;
	} BUFLOOP_END;
      }
      break;
    }
  }
}

// ----------------------------------------------------------------------
// writer_comm_destroy

static void
writer_comm_destroy(struct collective_m3_ctx *ctx)
{
  free(ctx->recv_reqs);
  
  for (struct collective_m3_peer *peer = ctx->peers; peer < ctx->peers + ctx->n_peers; peer++) {
    free(peer->recv_buf);
  }
  free(ctx->peers);

  free(ctx->recv_patches);
}

// ----------------------------------------------------------------------
// writer_comm_local

static void
writer_comm_local(struct collective_m3_ctx *ctx, struct mrc_io *io, struct mrc_ndarray *nd,
		  struct mrc_fld *m3, int m)
{
  int nr_patches;
  struct mrc_patch *patches = mrc_domain_get_patches(m3->_domain, &nr_patches);

  for (int p = 0; p < nr_patches; p++) {
    struct mrc_patch *patch = &patches[p];
    int *off = patch->off, *ldims = patch->ldims;

    int ilo[3], ihi[3];
    bool has_intersection =
      find_intersection(ilo, ihi, off, ldims, mrc_ndarray_offs(nd), mrc_ndarray_dims(nd));
    if (!has_intersection) {
      continue;
    }
    switch (mrc_fld_data_type(m3)) {
    case MRC_NT_FLOAT:
    {
      BUFLOOP(ix, iy, iz, ilo, ihi) {
        MRC_S3(nd, ix,iy,iz) =
          MRC_S5(m3, ix - off[0], iy - off[1], iz - off[2], m, p);
      } BUFLOOP_END
      break;
    }
    case MRC_NT_DOUBLE:
    {
      BUFLOOP(ix, iy, iz, ilo, ihi) {
        MRC_D3(nd, ix,iy,iz) =
          MRC_D5(m3, ix - off[0], iy - off[1], iz - off[2], m, p);
      } BUFLOOP_END
      break;     
    }
    case MRC_NT_INT:
    {
      BUFLOOP(ix, iy, iz, ilo, ihi) {
        MRC_I3(nd, ix,iy,iz) =
          MRC_I5(m3, ix - off[0], iy - off[1], iz - off[2], m, p);
      } BUFLOOP_END
      break;
    }
    default:
    {
      	assert(0);
    }
    }    
  }
}

// ----------------------------------------------------------------------
// collective_write_fld
// does the actual write of the partial fld to the file
// only called on writer procs

static void
collective_write_fld(struct collective_m3_ctx *ctx, struct mrc_io *io,
		    const char *path, struct mrc_ndarray *nd, int m,
		    struct mrc_fld *m3, struct xdmf_spatial *xs, hid_t group0)
{
  int ierr;

  char default_name[100];
  const char *compname;

  // If the comps aren't named just name them by their component number
  if ( !(compname = mrc_fld_comp_name(m3, m)) ) {
    sprintf(default_name, "_UNSET_%d", m);
    compname = (const char *) default_name;
  }

  xdmf_spatial_save_fld_info(xs, strdup(compname), strdup(path), false, mrc_fld_data_type(m3));

  hid_t group_fld = H5Gcreate(group0, compname, H5P_DEFAULT,
			      H5P_DEFAULT, H5P_DEFAULT); H5_CHK(group_fld);
  ierr = H5LTset_attribute_int(group_fld, ".", "m", &m, 1); CE;
  
  hid_t group = H5Gcreate(group_fld, "p0", H5P_DEFAULT,
			  H5P_DEFAULT, H5P_DEFAULT); H5_CHK(group);
  int i0 = 0;
  ierr = H5LTset_attribute_int(group, ".", "global_patch", &i0, 1); CE;

  hsize_t fdims[3] = { ctx->slab_dims[2], ctx->slab_dims[1], ctx->slab_dims[0] };
  hid_t filespace = H5Screate_simple(3, fdims, NULL); H5_CHK(filespace);
  hid_t dtype;
  switch (mrc_ndarray_data_type(nd)) {
  case MRC_NT_FLOAT: dtype = H5T_NATIVE_FLOAT; break;
  case MRC_NT_DOUBLE: dtype = H5T_NATIVE_DOUBLE; break;
  case MRC_NT_INT: dtype = H5T_NATIVE_INT; break;
  default: assert(0);
  }

  hid_t dset = H5Dcreate(group, "3d", dtype, filespace, H5P_DEFAULT,
			 H5P_DEFAULT, H5P_DEFAULT); H5_CHK(dset);
  hid_t dxpl = H5Pcreate(H5P_DATASET_XFER); H5_CHK(dxpl);
#ifdef H5_HAVE_PARALLEL
  struct xdmf *xdmf = to_xdmf(io);
  if (xdmf->use_independent_io) {
    ierr = H5Pset_dxpl_mpio(dxpl, H5FD_MPIO_INDEPENDENT); CE;
  } else {
    ierr = H5Pset_dxpl_mpio(dxpl, H5FD_MPIO_COLLECTIVE); CE;
  }
#endif
  const int *im = mrc_ndarray_dims(nd), *ib = mrc_ndarray_offs(nd);
  hsize_t mdims[3] = { im[2], im[1], im[0] };
  hsize_t foff[3] = { ib[2] - ctx->slab_off[2],
		      ib[1] - ctx->slab_off[1],
		      ib[0] - ctx->slab_off[0] };
  hid_t memspace = H5Screate_simple(3, mdims, NULL);
  ierr = H5Sselect_hyperslab(filespace, H5S_SELECT_SET, foff, NULL,
			     mdims, NULL); CE;

  ierr = H5Dwrite(dset, dtype, memspace, filespace, dxpl, nd->arr); CE;
  
  ierr = H5Dclose(dset); CE;
  ierr = H5Sclose(memspace); CE;
  ierr = H5Sclose(filespace); CE;
  ierr = H5Pclose(dxpl); CE;

  ierr = H5Gclose(group); CE;
  ierr = H5Gclose(group_fld); CE;
}

// ----------------------------------------------------------------------
// xdmf_collective_write_m3

void
xdmf_collective_write_m3(struct mrc_io *io, const char *path, struct mrc_fld *m3)
{
  struct xdmf *xdmf = to_xdmf(io);

  struct collective_m3_ctx ctx;
  collective_m3_init(io, &ctx, m3->_domain);

  struct xdmf_file *file = &xdmf->file;
  struct xdmf_spatial *xs = xdmf_spatial_find(&file->xdmf_spatial_list,
					      mrc_domain_name(m3->_domain));
  if (!xs) {
    xs = xdmf_spatial_create_m3_parallel(&file->xdmf_spatial_list,
					 mrc_domain_name(m3->_domain),
					 m3->_domain,
					 ctx.slab_off, ctx.slab_dims, io);
  }

  // If we have an aos field, we need to get it as soa for collection and writing
  struct mrc_fld *m3_soa = m3;
  if (m3->_aos) {
    switch (mrc_fld_data_type(m3)) {
      case MRC_NT_FLOAT: m3_soa = mrc_fld_get_as(m3, "float"); break;
      case MRC_NT_DOUBLE: m3_soa = mrc_fld_get_as(m3, "double"); break;
      case MRC_NT_INT: m3_soa = mrc_fld_get_as(m3, "int"); break;
      default: assert(0);
    }
  }

  if (xdmf->is_writer) {
    int writer;
    MPI_Comm_rank(xdmf->comm_writers, &writer);
    int writer_dims[3], writer_off[3];
    get_writer_off_dims(&ctx, writer, writer_off, writer_dims);
    mprintf("writer_off %d %d %d dims %d %d %d\n",
    	    writer_off[0], writer_off[1], writer_off[2],
    	    writer_dims[0], writer_dims[1], writer_dims[2]);

    struct mrc_ndarray *nd = mrc_ndarray_create(MPI_COMM_NULL);
    switch (mrc_fld_data_type(m3)) {
    case MRC_NT_FLOAT: mrc_ndarray_set_type(nd, "float"); break;
    case MRC_NT_DOUBLE: mrc_ndarray_set_type(nd, "double"); break;
    case MRC_NT_INT: mrc_ndarray_set_type(nd, "int"); break;
    default: assert(0);
    }
    mrc_ndarray_set_param_int_array(nd, "dims", 3, writer_dims);
    mrc_ndarray_set_param_int_array(nd, "offs", 3, writer_off);
    mrc_ndarray_setup(nd);

    hid_t group0;
    if (H5Lexists(file->h5_file, path, H5P_DEFAULT) > 0) {
      group0 = H5Gopen(file->h5_file, path, H5P_DEFAULT);
    } else {
      assert(0); // FIXME, can this happen?
      group0 = H5Gcreate(file->h5_file, path, H5P_DEFAULT,
			 H5P_DEFAULT, H5P_DEFAULT); H5_CHK(group0);
    }
    int nr_1 = 1;
    H5LTset_attribute_int(group0, ".", "nr_patches", &nr_1, 1);

    writer_comm_init(&ctx, io, nd, m3_soa->_domain, m3_soa->_nd->size_of_type);
    for (int m = 0; m < mrc_fld_nr_comps(m3); m++) {
      writer_comm_begin(&ctx, io, nd, m3_soa);
      collective_send_fld_begin(&ctx, io, m3_soa, 0);
      writer_comm_local(&ctx, io, nd, m3_soa, 0);
      writer_comm_end(&ctx, io, nd, m3_soa, 0);
      //collective_write_fld(&ctx, io, path, nd, m, m3, xs, group0);
      collective_send_fld_end(&ctx, io, m3, 0);
    }
    writer_comm_destroy(&ctx);

    H5Gclose(group0);
    mrc_ndarray_destroy(nd);
  } else {
    for (int m = 0; m < mrc_fld_nr_comps(m3); m++) {
      collective_send_fld_begin(&ctx, io, m3_soa, m);
      collective_send_fld_end(&ctx, io, m3_soa, m);
    }
  }

  if (m3->_aos) {
    mrc_fld_put_as(m3_soa, m3);
  }
}

