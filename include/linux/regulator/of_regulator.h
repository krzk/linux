/*
 * OpenFirmware regulator support routines
 *
 */

#ifndef __LINUX_OF_REG_H
#define __LINUX_OF_REG_H

struct regulator_desc;
struct regulator_bulk_data;

struct of_regulator_match {
	const char *name;
	void *driver_data;
	struct regulator_init_data *init_data;
	struct device_node *of_node;
	const struct regulator_desc *desc;
};

#if defined(CONFIG_OF)
extern struct regulator_init_data
	*of_get_regulator_init_data(struct device *dev,
				    struct device_node *node,
				    const struct regulator_desc *desc);
extern int of_regulator_match(struct device *dev, struct device_node *node,
			      struct of_regulator_match *matches,
			      unsigned int num_matches);
extern int devm_of_regulator_all_get(struct device *dev,
				     unsigned int *num_consumers,
				     struct regulator_bulk_data **consumers);
#else
static inline struct regulator_init_data
	*of_get_regulator_init_data(struct device *dev,
				    struct device_node *node,
				    const struct regulator_desc *desc)
{
	return NULL;
}

static inline int of_regulator_match(struct device *dev,
				     struct device_node *node,
				     struct of_regulator_match *matches,
				     unsigned int num_matches)
{
	return 0;
}
static inline int devm_of_regulator_all_get(struct device *dev,
					    unsigned int *num_consumers,
					    struct regulator_bulk_data **consumers)
{
	*num_consumers = 0;
	*consumers = NULL;

	return 0;
}
#endif /* CONFIG_OF */

#endif /* __LINUX_OF_REG_H */
