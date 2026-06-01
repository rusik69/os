#ifndef IOMMU_H
#define IOMMU_H

#include "types.h"

/* IOMMU domain — opaque (stub implementation) */
struct iommu_domain {
    uint64_t reserved;
};

/* IOMMU access flags */
#define IOMMU_READ   (1UL << 0)
#define IOMMU_WRITE  (1UL << 1)
#define IOMMU_EXEC   (1UL << 2)

/* Allocate an IOMMU domain. Returns NULL (stub — no IOMMU present). */
static inline struct iommu_domain *iommu_domain_alloc(void) {
    return NULL;
}

/* Map a physical region into an IOMMU domain.
   Returns 0 on success (stub always returns 0, does nothing). */
static inline int iommu_map(struct iommu_domain *domain,
                             uint64_t iova, uint64_t phys_addr,
                             size_t size, uint64_t flags) {
    (void)domain;
    (void)iova;
    (void)phys_addr;
    (void)size;
    (void)flags;
    return 0;
}

/* Unmap a region from an IOMMU domain.
   Returns 0 on success (stub always returns 0). */
static inline int iommu_unmap(struct iommu_domain *domain,
                               uint64_t iova, size_t size) {
    (void)domain;
    (void)iova;
    (void)size;
    return 0;
}

/* Free an IOMMU domain. */
static inline void iommu_domain_free(struct iommu_domain *domain) {
    (void)domain;
}

#endif /* IOMMU_H */
